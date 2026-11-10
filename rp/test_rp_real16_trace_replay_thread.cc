#include "test_rp.hh"
#include "iommu_top.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// 16 盘实测 TLP 数据回放场景 (real16_trace_replay)
//
// 输入: 工程根目录两份实测 xlsx (MRd / MWr) 经 rp/prepare_real16_trace.py
//       预处理为 4 份 CSV —— 组A(did 0..7): read_trace.csv / write_trace.csv;
//       组B(did 8..15): read_trace_1.csv / write_trace_1.csv, 组B 为组A 的
//       内存虚拟副本(等价 request id 逐行+8), len/addr/行序与组A完全相同。
//       由本线程在初始化阶段加载。
//
// 关键口径(与计划文档逐条对应):
//   - 设备固定 16 台: 组A RequesterID 205/206/207/208/211/212/213/214 -> DID 0..7;
//     组B = 同一白名单 +8 -> DID 8..15;
//   - 每方向逐包交替轮询合并两组(A[0],B[0],A[1],B[1]...), 模拟 16 设备公共交错发包;
//   - 两组 4KB 页映射完全独立: 全局有效页表键 = (组别<<32)|原始 Address 低32位页号,
//     跨组同原始地址必得不同 generation、不同 IOVA 页, 不复用;
//     组内语义与 8 盘一致(跨设备/方向共享一代, 4096B 覆盖后立即失效);
//   - IOVA 512MiB(0x100000 起, 131072 页), 随机无放回池预留末端 D=3 页;
//     单池单游标顺序分配, 跨组页号天然全局不重复;
//   - 注入地址 = 映射页 + 原始页内偏移; S1/S2 均由 RP 预先建好, 翻译结果可校验;
//   - 读写各一条常驻发送进程, 不互相等待, 包间不额外 wait, 节拍与反压由端口决定;
//   - 无人工哨兵/预热请求, 正式回放冷启动; 结束条件为两方向发完 + 全部响应 +
//     IOMMU 内部 quiescent。
//
// 本文件仅在 TEST=real16_trace_replay 时经 TEST_THREAD_SRC 编入。
// 说明: iommu 侧占用采样钩子以 TEST_CFG_REAL8_TRACE_REPLAY 为场景宏(命名沿袭),
//       本构建同样定义该宏, 因此无需修改 iommu/ 任何代码。
// ============================================================================

#ifndef TEST_CFG_REAL8_TRACE_REPLAY
#define TEST_CFG_REAL8_TRACE_REPLAY 0
#endif

#if TEST_CFG_REAL8_TRACE_REPLAY && !TEST_CFG_MULTI_DEVICE_SCENE
#error "TEST_CFG_REAL8_TRACE_REPLAY 需要 TEST_CFG_MULTI_DEVICE_SCENE=1 (复用 md_trace)"
#endif
#if TEST_CFG_REAL8_TRACE_REPLAY && !TEST_CFG_SPLIT_RW_PORTS
#error "TEST_CFG_REAL8_TRACE_REPLAY 需要 TEST_CFG_SPLIT_RW_PORTS=1 (读写双口)"
#endif

#if TEST_CFG_REAL8_TRACE_REPLAY
namespace {
using iommu_md::require;

// ------------------------------ 固定配置 ------------------------------
constexpr unsigned N = TEST_CFG_NUM_DEVICES;          // 设备数(固定 16)
constexpr unsigned GROUP_SPLIT = 8;                   // 组A=DID[0,8), 组B=DID[8,16)
constexpr uint64_t IOVA_BASE = 0x100000ULL;           // IOVA 池基址
constexpr unsigned RANGE_MB = 512;                    // IOVA 范围 512MiB
constexpr unsigned RANGE_PAGES = RANGE_MB * 256;      // 131072 个 4KB 页
// 预取深度 D 的邻居页必须落在 IOVA 范围内: 主请求只在 [0, POOL_PAGES) 取页。
constexpr unsigned POOL_PAGES = RANGE_PAGES - PT_DEDUP_PREFETCH_DEPTH;
constexpr unsigned HUGE_COUNT = 50;                   // 50 个 2MiB GPA 大页
constexpr uint64_t HUGE = 0x200000ULL;
constexpr unsigned SLOTS_PER_HUGE = 512;              // 每个 2MiB 大页 512 个 4KB slot
constexpr uint64_t SPA_BASE = 0x80000000ULL;          // G-stage 之后的 SPA 基址
constexpr uint64_t SPA_STRIDE = 0x10000000ULL;        // 每设备 SPA 段偏移
// 无进展超时: 16 盘每方向注入约 12MB(读)/8MB(写), 理论带宽时间远小于 5ms。
constexpr double NO_PROGRESS_LIMIT_NS = 5000000.0;    // 单次无进展上限 5ms
constexpr double TOTAL_LIMIT_NS = 2000000000.0;       // 总时限 2s 兜底(仅诊断)
constexpr uint64_t PAGE_POOL_SEED = 20260421ULL;      // IOVA 页池排列种子(固定并记录)

uint64_t spa_offset(unsigned d) { return SPA_BASE + uint64_t(d) * SPA_STRIDE; }

// ------------------------------ 输入记录 ------------------------------
// read_trace(.1).csv / write_trace(.1).csv 的列序:
//   excel_row,did,tlp_type,dw,bytes,is_ctrl,addr_raw,addr_low32,page_base,offset
struct Rec {
    uint32_t excel_row = 0;
    uint32_t did = 0;
    uint32_t dw = 0;
    uint32_t bytes = 0;
    uint32_t is_ctrl = 0;
    uint32_t low32 = 0;
    uint32_t page_base = 0;
    uint32_t offset = 0;
    uint32_t page_no = 0;     // page_base >> 12
    uint32_t group = 0;       // 0=组A(did<8), 1=组B(did>=8); 两组映射完全独立
};

struct TraceStats {
    uint64_t rows = 0, bytes = 0;
    std::map<unsigned, uint64_t> per_dev_rows, per_dev_bytes;
    std::map<unsigned, uint64_t> per_len_rows;
    uint64_t ctrl_rows = 0, data_rows = 0;
};

// ------------------------------ 有效页生命周期 ------------------------------
// 一代配对: 由某个(组别, 原始 4KB 页)首次出现时创建, 全部 4096 字节覆盖后立即失效。
struct Generation {
    uint64_t mapped_page = 0;                       // 从全局 IOVA 池分配的页号
    uint64_t gen_id = 0;                            // 代编号(报告用)
    uint32_t group = 0;                             // 组别(页表复合键高位)
    uint32_t source_page_no = 0;                    // 原始 Address 低32位页号
    std::array<uint64_t, 64> bitmap{};              // 4096 bit 覆盖位图
    uint32_t covered = 0;                           // 已覆盖字节数
    uint32_t requests = 0;                          // 绑定到本代的请求数
    bool removed = false;                           // 是否已从可查询表移除
};

// 配对归档记录(代结束时写入, 不保留位图, 内存恒定)
struct PairRec {
    uint64_t gen_id = 0;
    uint32_t group = 0;
    uint32_t source_page_no = 0;
    uint32_t mapped_page = 0;
    uint32_t covered = 0;
    uint32_t requests = 0;
    uint8_t  complete = 0;
};

// ------------------------------ 场景主体 ------------------------------
class Real16TraceScenario {
public:
    RP_Module& rp;
    iommu_top& top;

    std::vector<Rec> recs[2];        // 0=读方向, 1=写方向(已按 A/B 逐包交替合并)
    TraceStats stats[2];             // 合并后总统计(did 0..15)
    TraceStats gstats[2][2];         // [dir][group] 组级统计(加载时打印对账)

    std::vector<uint32_t> pool;      // IOVA 页号随机无放回排列(长度 POOL_PAGES)
    uint32_t pool_next = 0;

    // 全局有效页表: 键 = (组别<<32)|原始 Address 低32位页号。
    // 组内跨设备/方向共享一代; 跨组同原始地址必得不同代、不同 IOVA 页。
    std::unordered_map<uint64_t, std::shared_ptr<Generation>> page_table;
    uint64_t gen_seq = 0, gen_created = 0, gen_completed = 0;
    std::vector<PairRec> pair_log;

    // 逐设备上下文与映射表: IOVA 页号 -> 该设备上下文下的 GPA
    std::array<device_context_t, N> dcs{};
    std::array<uint64_t, N> dc_address{};
    std::array<std::vector<uint64_t>, N> gpas;

    std::vector<std::unique_ptr<tlm_generic_payload>> payloads;
    std::vector<PayloadExtention*> payload_exts;
    std::unique_ptr<std::ofstream> remap_out;

    uint64_t total_responded = 0;
    uint64_t total_requests = 0;
    bool stopped = false;
    bool send_done[2] = {false, false};
    uint64_t sent[2] = {0, 0};
    uint64_t sent_bytes[2] = {0, 0};
    uint64_t resp[2] = {0, 0};
    uint64_t resp_bytes[2] = {0, 0};
    double last_send_ns[2] = {0.0, 0.0};
    double first_send_ns[2] = {-1.0, -1.0};
    double start_ns = 0.0;
    uint64_t timeout_diag = 0;
    uint64_t cursor[2] = {0, 0};    // 当前发送行号(超时诊断用)

    explicit Real16TraceScenario(RP_Module& r)
        : rp(r), top(*r.iommu_ptr) {}

    ~Real16TraceScenario() {
        // 正常结束只在所有响应及异步工作排空后清理。
        for (size_t i = 0; i < payloads.size(); ++i) {
            PayloadExtention* ext = payload_exts[i];
            if (!ext) continue;
            payloads[i]->clear_extension(ext);
            delete ext;
        }
    }

    // 页表复合键: 组别独立映射的核心 —— 同一原始页在两组各建一代。
    static uint64_t pkey(uint32_t group, uint32_t page_no) {
        return (uint64_t(group) << 32) | uint64_t(page_no);
    }

    // ---------------------------- CSV 加载 ----------------------------
    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> out;
        size_t begin = 0;
        while (true) {
            size_t pos = line.find(',', begin);
            if (pos == std::string::npos) { out.push_back(line.substr(begin)); break; }
            out.push_back(line.substr(begin, pos - begin));
            begin = pos + 1;
        }
        return out;
    }
    static uint64_t to_u64(const std::string& s, const char* what, uint64_t line_no) {
        char* end = nullptr;
        errno = 0;
        unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        require(end && *end == '\0' && errno == 0,
                std::string("CSV字段解析失败: ") + what + " 行" + std::to_string(line_no) +
                " 值'" + s + "'");
        return static_cast<uint64_t>(v);
    }
    static uint32_t to_hex32(const std::string& s, const char* what, uint64_t line_no) {
        char* end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(s.c_str(), &end, 16);
        require(end && *end == '\0' && errno == 0,
                std::string("CSV十六进制字段解析失败: ") + what + " 行" +
                std::to_string(line_no) + " 值'" + s + "'");
        return static_cast<uint32_t>(v);
    }
    void load_csv(const std::string& path, unsigned dir, unsigned group,
                  std::vector<Rec>& out, TraceStats& st) {
        std::ifstream in(path);
        require(in.good(), "无法打开规范化输入: " + path);
        std::string line;
        require(static_cast<bool>(std::getline(in, line)), "规范化输入缺少表头: " + path);
        const auto expect = split(line);
        require(expect.size() == 10 && expect[0] == "excel_row" && expect[1] == "did" &&
                expect[3] == "dw" && expect[4] == "bytes" && expect[5] == "is_ctrl" &&
                expect[7] == "addr_low32" && expect[8] == "page_base" && expect[9] == "offset",
                "规范化输入表头不符: " + path);
        uint64_t line_no = 1;
        while (std::getline(in, line)) {
            ++line_no;
            if (line.empty()) continue;
            const auto f = split(line);
            require(f.size() == 10, "规范化输入列数不足: " + path + " 行" +
                                    std::to_string(line_no));
            Rec r;
            r.excel_row = static_cast<uint32_t>(to_u64(f[0], "excel_row", line_no));
            r.did = static_cast<uint32_t>(to_u64(f[1], "did", line_no));
            r.dw = static_cast<uint32_t>(to_u64(f[3], "dw", line_no));
            r.bytes = static_cast<uint32_t>(to_u64(f[4], "bytes", line_no));
            r.is_ctrl = static_cast<uint32_t>(to_u64(f[5], "is_ctrl", line_no));
            r.low32 = to_hex32(f[7], "addr_low32", line_no);
            r.page_base = to_hex32(f[8], "page_base", line_no);
            r.offset = static_cast<uint32_t>(to_u64(f[9], "offset", line_no));
            require(r.did < N, "CSV 设备号越界: " + path + " 行" + std::to_string(line_no));
            r.group = (r.did >= GROUP_SPLIT) ? 1u : 0u;
            require(r.group == group, "CSV 设备号与文件组别不符: " + path +
                                      " 行" + std::to_string(line_no));
            require(r.bytes > 0 && r.bytes <= 4096 && (r.bytes % 4) == 0,
                    "CSV 长度非法: " + path + " 行" + std::to_string(line_no));
            require(r.offset + r.bytes <= 4096,
                    "CSV 请求跨 4KB 页: " + path + " 行" + std::to_string(line_no));
            require((r.low32 & ~0xFFFu) == r.page_base,
                    "CSV 页基址与低32位地址不一致: " + path + " 行" + std::to_string(line_no));
            require((dir == 1) ? (r.is_ctrl == (r.bytes == 16 ? 1u : 0u))
                               : (r.is_ctrl == 0u),
                    "CSV DATA/CTRL 标记与方向规则不符: " + path + " 行" +
                    std::to_string(line_no));
            r.page_no = r.page_base >> 12;
            out.push_back(r);

            st.rows++;
            st.bytes += r.bytes;
            st.per_dev_rows[r.did]++;
            st.per_dev_bytes[r.did] += r.bytes;
            st.per_len_rows[r.bytes]++;
            if (r.is_ctrl) st.ctrl_rows++; else st.data_rows++;
        }
        require(!out.empty(), "规范化输入没有数据行: " + path);
        std::cout << "[REAL16_INPUT] " << (dir == 0 ? "read" : "write")
                  << " group=" << (group ? "B" : "A") << " file=" << path
                  << " rows=" << st.rows << " bytes=" << st.bytes
                  << " data=" << st.data_rows << " ctrl=" << st.ctrl_rows << '\n';
        fflush(stdout);
    }

    // 每方向逐包交替轮询合并两组: A[0],B[0],A[1],B[1]...; 等长为常规情形。
    void merge_group(unsigned dir, const std::string& pa, const std::string& pb) {
        std::vector<Rec> a, b;
        load_csv(pa, dir, 0, a, gstats[dir][0]);
        load_csv(pb, dir, 1, b, gstats[dir][1]);
        require(a.size() == b.size(),
                "两组记录数不一致(组B为组A虚拟副本, 必须等长): dir=" +
                std::to_string(dir) + " A=" + std::to_string(a.size()) +
                " B=" + std::to_string(b.size()));
        auto& v = recs[dir];
        v.clear();
        v.reserve(a.size() + b.size());
        auto& st = stats[dir];
        for (size_t i = 0; i < a.size(); ++i) {
            v.push_back(a[i]);
            v.push_back(b[i]);
        }
        // 合并统计: 直接聚合两组(避免二次遍历大向量的重复逻辑)。
        st = gstats[dir][0];
        const auto& gb = gstats[dir][1];
        st.rows += gb.rows;
        st.bytes += gb.bytes;
        st.ctrl_rows += gb.ctrl_rows;
        st.data_rows += gb.data_rows;
        for (const auto& kv : gb.per_dev_rows) st.per_dev_rows[kv.first] += kv.second;
        for (const auto& kv : gb.per_dev_bytes) st.per_dev_bytes[kv.first] += kv.second;
        for (const auto& kv : gb.per_len_rows) st.per_len_rows[kv.first] += kv.second;
        std::cout << "[REAL16_MERGE] dir=" << (dir == 1 ? "W" : "R")
                  << " interleave=A/B rows=" << v.size()
                  << " bytes=" << st.bytes << '\n';
        fflush(stdout);
    }

    // ---------------------------- 页表构建 ----------------------------
    void map_vs(unsigned d, uint64_t iova, uint64_t gpa) {
        spte_t p{};
        p.V = p.R = p.W = p.U = p.A = p.D = 1;
        p.PBMT = PMA;
        p.PPN = gpa / 4096;
        require(rp.add_vs_stage_pte(&top, dcs[d].fsc.iosatp, iova, p, 0,
                                    dcs[d].iohgatp, 0) != uint64_t(-1),
                "构建 VS 页表失败 did=" + std::to_string(d) + " iova=" + std::to_string(iova));
    }
    void map_g(unsigned d, uint64_t gpa, uint64_t spa, unsigned level) {
        gpte_t p{};
        p.V = p.R = p.W = p.U = p.A = p.D = 1;
        p.PBMT = PMA;
        p.PPN = spa / 4096;
        require(rp.add_g_stage_pte(&top, dcs[d].iohgatp, gpa, p,
                                   static_cast<uint8_t>(level)) != uint64_t(-1),
                "构建 G-stage 失败 did=" + std::to_string(d));
    }

    // 全局失效: 建表后清空 IOMMU 内部 DC/PC/PT/Walker Cache, 保证冷启动只依赖真实页表。
    void invalidate_all() {
        iommu::CacheMessage cmd;
        cmd.msg_type = iommu::CacheMsgType::DC_INVALIDATE;
        cmd.cmd_type = iommu::InvalidCmdType::GLOBAL_INVAL;
        cmd.invalidate_mode = iommu::CacheInvalidateMode::GLOBAL;
        top.cache_sub.invalidation_request_fifo.write(cmd);
        auto response = top.cache_sub.invalidation_response_fifo.read();
        require(response.msg_type == iommu::CacheMsgType::CACHE_INVALIDATE_RESPONSE,
                "全局失效未完成");
        top.md_trace.progress();
    }

    void setup() {
        // 保留现有低地址寄存器/CQ 区域, 只初始化一次物理分配游标。
        next_free_page = std::max<uint64_t>(next_free_page, 240);
        // 最坏页表预算: 512MiB/2MiB = 256 张 VS L0 + 3 张上级目录 + G-stage 目录及裕量。
        const uint64_t per_device_pages = 256 + 8 + 16;
        require((next_free_page + 1 + uint64_t(N) * per_device_pages) * 4096 <=
                    DDR_Module::DDR_MEMORY_SIZE,
                "所有设备页表预算超过 32MiB DDR");
        require(rp.enable_iommu(&top, DDT_1LVL) >= 0, "启用 IOMMU 失败");

        // IOVA 页池: [0, POOL_PAGES) 的随机无放回排列; 末端 D 页留给预取邻居,
        // 永远不会被主请求分配, 从而保证 page+k (k<=D) 的 VS 映射总是存在。
        // 单池单游标: 两组各自建代时顺序取页, 跨组页号天然全局不重复。
        pool.resize(POOL_PAGES);
        for (unsigned i = 0; i < POOL_PAGES; ++i) pool[i] = i;
        std::mt19937 shuffle_rng(static_cast<uint32_t>(PAGE_POOL_SEED));
        std::shuffle(pool.begin(), pool.end(), shuffle_rng);

        for (unsigned d = 0; d < N; ++d) {
            // 每设备独立上下文: DID=d, GSCID=d+1, PSCID=0; MSIPTP_Off 保证普通数据
            // GPA 不会被 MSI 地址域误识别(本场景不合成 MSI)。
            next_free_gpage[d + 1] = 0x10000;
            dc_address[d] = rp.add_device(&top, d, d + 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0,
                                          IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                          MSIPTP_Off, 0, 0, 0);
            require(dc_address[d] != uint64_t(-1), "添加设备失败 did=" + std::to_string(d));
            rp.read_memory_test_rp(dc_address[d], sizeof(dcs[d]),
                                   reinterpret_cast<char*>(&dcs[d]));
            dcs[d].ta.PSCID = 0;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&dcs[d]), dc_address[d],
                                    sizeof(dcs[d]));
            device_context_t readback{};
            rp.read_memory_test_rp(dc_address[d], sizeof(readback),
                                   reinterpret_cast<char*>(&readback));
            require(readback.iohgatp.GSCID == d + 1 && readback.ta.PSCID == 0 &&
                    readback.tc.PDTV == 0, "DC 读回上下文不匹配 did=" + std::to_string(d));

            // 每设备独立 GPA 映射: 50 个 2MiB 大页, 页内随机 4KB slot, 种子 42+d。
            gpas[d].resize(RANGE_PAGES);
            std::mt19937 mapping(42u + d);
            for (unsigned i = 0; i < RANGE_PAGES; ++i) {
                const unsigned h = mapping() % HUGE_COUNT;
                const unsigned slot = mapping() % SLOTS_PER_HUGE;
                gpas[d][i] = uint64_t(h) * HUGE + uint64_t(slot) * 4096;
            }
            // G-stage: 50 个 2MiB 大页整体映射到本设备 SPA 段。
            for (unsigned h = 0; h < HUGE_COUNT; ++h) {
                map_g(d, uint64_t(h) * HUGE, uint64_t(h) * HUGE + spa_offset(d), 1);
            }
            // VS: 512MiB 全范围建立 4KB 映射(覆盖全部预取邻居)。
            for (unsigned i = 0; i < RANGE_PAGES; ++i) {
                map_vs(d, IOVA_BASE + uint64_t(i) * 4096, gpas[d][i]);
            }
            std::cout << "[REAL16_SETUP] DID=" << d
                      << " group=" << (d >= GROUP_SPLIT ? "B" : "A")
                      << " GSCID=" << (d + 1) << " PSCID=0"
                      << " iova_pages=" << RANGE_PAGES
                      << " gpa_huge_pages=" << HUGE_COUNT
                      << " spa_base=0x" << std::hex << spa_offset(d) << std::dec
                      << " free_ppn=" << next_free_page << '\n';
            fflush(stdout);
            top.md_trace.progress();
        }
        invalidate_all();
    }

    // ---------------------------- 请求发送 ----------------------------
    static unsigned char* shared_data() {
        static unsigned char buffer[4096] = {0};
        return buffer;
    }

    void emit(unsigned dir, size_t idx) {
        const Rec& rec = recs[dir][idx];
        // ===== 无 wait 临界区: 查询/建立(组别,原始页)有效页表 =====
        std::shared_ptr<Generation> gen;
        const uint64_t key = pkey(rec.group, rec.page_no);
        auto it = page_table.find(key);
        if (it != page_table.end()) {
            gen = it->second;
        } else {
            require(pool_next < POOL_PAGES,
                    "IOVA 页池耗尽(禁止循环复用): 已分配 " + std::to_string(pool_next) +
                    " 页, 上限 " + std::to_string(POOL_PAGES));
            gen = std::make_shared<Generation>();
            gen->mapped_page = pool[pool_next++];
            gen->gen_id = ++gen_seq;
            gen->group = rec.group;
            gen->source_page_no = rec.page_no;
            page_table.emplace(key, gen);
            ++gen_created;
        }
        ++gen->requests;

        const uint64_t iova = IOVA_BASE + gen->mapped_page * 4096ULL + rec.offset;
        const uint64_t expected_pa = gpas[rec.did][gen->mapped_page] + rec.offset +
                                     spa_offset(rec.did);

        // ===== 构造 payload(与既有双端口场景同构) =====
        auto trans = std::make_unique<tlm_generic_payload>();
        trans->set_address(iova);
        trans->set_data_ptr(shared_data());
        trans->set_data_length(rec.bytes);
        trans->set_streaming_width(rec.bytes);
        trans->set_command((dir == 1) ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
        trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
        auto* ext = new PayloadExtention();
        ext->requester_id = rec.did;
        ext->pid_valid = 0;
        ext->process_id = 0;
        ext->exec_req = 0;
        ext->priv_req = 0;
        ext->no_write = (dir == 1) ? 0 : 1;
        ext->at = 0;
        ext->is_ctrl = rec.is_ctrl ? 1u : 0u;
        trans->set_extension(ext);

        tlm_generic_payload* ptr = trans.get();
        payloads.push_back(std::move(trans));
        payload_exts.push_back(ext);

        iommu_md::Request record;
        record.device = rec.did;
        record.epoch = iommu_md::MEASUREMENT_EPOCH;
        record.sequence = idx;
        record.group = gen->gen_id;            // 配对代编号
        record.type = rec.is_ctrl ? 1u : 0u;   // 0=DATA(任意长度) 1=CTRL
        record.bytes = rec.bytes;
        record.write = (dir == 1);
        record.iova = iova;
        record.expected_pa = expected_pa;
        record.ready_ns = iommu_md::now_ns();
        top.md_trace.register_request(ptr, record);

        // 逐包重映射记录(流式写盘; dir+excel_row 与规范化输入逐行对应, seq 与 requests.csv 关联)
        if (remap_out) {
            *remap_out << (dir == 1 ? 'W' : 'R') << ',' << (unsigned)rec.group << ','
                       << rec.excel_row << ',' << rec.did
                       << ',' << rec.bytes << ',' << rec.is_ctrl << ','
                       << (rec.page_base >> 12) << ',' << rec.offset << ',' << gen->gen_id
                       << ',' << gen->mapped_page << ',' << gpas[rec.did][gen->mapped_page]
                       << ',' << expected_pa << '\n';
        }

        if (first_send_ns[dir] < 0.0) first_send_ns[dir] = iommu_md::now_ns();

        // ===== 端口调用(带宽节拍 + 并发门控在此阻塞本方向) =====
        sc_time delay = SC_ZERO_TIME;
        tlm_phase phase = BEGIN_REQ;
        tlm::tlm_sync_enum status = (dir == 1)
            ? rp.axi_master_to_pcie_noc_0w_socket->nb_transport_fw(*ptr, phase, delay)
            : rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr, phase, delay);
        require(status == TLM_UPDATED && phase == END_REQ,
                "端口握手异常(禁止重发): dir=" + std::to_string(dir) +
                " excel_row=" + std::to_string(rec.excel_row));

        const double now = iommu_md::now_ns();
        top.md_trace.request(ptr).grant_ns = now;
        last_send_ns[dir] = now;
        ++sent[dir];
        sent_bytes[dir] += rec.bytes;

        // ===== 无 wait 临界区: 覆盖并账 + 完整后立即移除该代 =====
        accumulate(gen, rec.offset, rec.bytes);
    }

    // 位图 OR 并账: 仅新覆盖字节计入 covered; 满 4096 时立即"删除该代"。
    // 已进入端口调用的另一方向请求持有 shared_ptr, 代对象不会悬空, 也不会错绑新代。
    void accumulate(const std::shared_ptr<Generation>& gen, uint32_t offset, uint32_t bytes) {
        const uint32_t first_word = offset >> 6;
        const uint32_t last_word = (offset + bytes - 1) >> 6;
        for (uint32_t w = first_word; w <= last_word; ++w) {
            const uint32_t lo = (w == first_word) ? (offset & 63u) : 0u;
            const uint32_t hi = (w == last_word) ? (((offset + bytes - 1) & 63u) + 1u) : 64u;
            const uint64_t low_mask = (lo == 0u) ? ~0ULL : (~0ULL << lo);
            const uint64_t high_mask = (hi >= 64u) ? ~0ULL : ((1ULL << hi) - 1ULL);
            const uint64_t before = gen->bitmap[w];
            const uint64_t after = before | (low_mask & high_mask);
            if (after != before) {
                gen->covered += static_cast<uint32_t>(__builtin_popcountll(after & ~before));
                gen->bitmap[w] = after;
            }
        }
        if (!gen->removed && gen->covered >= 4096u) {
            gen->removed = true;                    // 该代不再可被后续请求查询
            auto it = page_table.find(pkey(gen->group, gen->source_page_no));
            if (it != page_table.end() && it->second.get() == gen.get()) {
                page_table.erase(it);               // 立即从可查询表移除
            }
            PairRec r;
            r.gen_id = gen->gen_id;
            r.group = gen->group;
            r.source_page_no = gen->source_page_no;
            r.mapped_page = static_cast<uint32_t>(gen->mapped_page);
            r.covered = gen->covered;
            r.requests = gen->requests;
            r.complete = 1;
            pair_log.push_back(r);
            ++gen_completed;
        }
    }

    void sender(unsigned dir) {
        const size_t total = recs[dir].size();
        for (size_t i = 0; i < total; ++i) {
            if (stopped) return;
            cursor[dir] = i;
            emit(dir, i);
            if ((i & 0x3FFF) == 0) top.md_trace.progress();  // 进度标记(watchdog 用)
        }
        send_done[dir] = true;
        top.md_trace.progress();
        std::cout << "[REAL16_SEND_END] dir=" << (dir == 1 ? "W" : "R")
                  << " rows=" << total << " bytes=" << sent_bytes[dir]
                  << " first_send=" << first_send_ns[dir]
                  << " last_send=" << last_send_ns[dir] << '\n';
        fflush(stdout);
    }

    // ---------------------------- 响应 ----------------------------
    // 按 payload 身份校验 DID / 长度 / 状态 / expected SPA 及恰好一次完成。
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);
        require(ext && ext->requester_id < N, "响应缺少有效设备ID");
        auto& r = top.md_trace.request(&trans);
        require(r.epoch == iommu_md::MEASUREMENT_EPOCH, "收到非测量期的响应");
        require(static_cast<uint32_t>(trans.get_data_length()) == r.bytes,
                "响应长度与请求不一致 seq=" + std::to_string(r.sequence));
        require(trans.get_response_status() == tlm::TLM_OK_RESPONSE,
                "响应状态非 OK seq=" + std::to_string(r.sequence));
        top.md_trace.response(trans, ext->requester_id);   // 校验 expected SPA 且防重复
        ++total_responded;
        const unsigned dir = r.write ? 1u : 0u;
        ++resp[dir];
        resp_bytes[dir] += r.bytes;
    }

    // ---------------------------- 收尾 ----------------------------
    void collect_residuals() {
        uint64_t covered_sum = 0, gap_sum = 0, req_sum = 0;
        for (const auto& kv : page_table) {
            const auto& g = kv.second;
            if (g->covered >= 4096u) continue;
            PairRec r;
            r.gen_id = g->gen_id;
            r.group = g->group;
            r.source_page_no = g->source_page_no;
            r.mapped_page = static_cast<uint32_t>(g->mapped_page);
            r.covered = g->covered;
            r.requests = g->requests;
            r.complete = 0;
            pair_log.push_back(r);
            covered_sum += g->covered;
            gap_sum += (4096u - g->covered);
            req_sum += g->requests;
        }
        std::cout << "[REAL16_RESIDUAL] pairs=" << (pair_log.size() - gen_completed)
                  << " covered_bytes=" << covered_sum
                  << " gap_bytes=" << gap_sum
                  << " requests=" << req_sum << '\n';
        fflush(stdout);
    }

    void write_pairs(const std::string& prefix) {
        std::ofstream out(prefix + "_pairs.csv");
        require(out.good(), "无法创建配对 CSV");
        out << "gen_id,group,source_page_no,mapped_page,covered_bytes,requests,complete\n";
        for (const auto& r : pair_log) {
            out << r.gen_id << ',' << (unsigned)r.group << ',' << r.source_page_no << ','
                << r.mapped_page << ',' << r.covered << ',' << r.requests << ','
                << (unsigned)r.complete << '\n';
        }
    }

    void watchdog() {
        double mark = iommu_md::now_ns();
        uint64_t last_serial = top.md_trace.progress_serial;
        while (!stopped) {
            wait(1000, SC_NS);
            const double now = iommu_md::now_ns();
            if (top.md_trace.progress_serial != last_serial) {
                last_serial = top.md_trace.progress_serial;
                mark = now;
            }
            const bool no_progress = (now - mark) > NO_PROGRESS_LIMIT_NS;
            const bool too_long = (now - start_ns) > TOTAL_LIMIT_NS;
            if (no_progress || too_long) {
                ++timeout_diag;
                std::cerr << "[REAL16_TIMEOUT] kind=" << (no_progress ? "no_progress" : "total_limit")
                          << " t=" << now << " stuck_ns=" << (now - mark)
                          << " responded=" << total_responded << "/" << total_requests
                          << " sent_r=" << sent[0] << "/" << recs[0].size()
                          << " cursor_r=" << cursor[0]
                          << " sent_w=" << sent[1] << "/" << recs[1].size()
                          << " cursor_w=" << cursor[1]
                          << " send_done_r=" << send_done[0]
                          << " send_done_w=" << send_done[1] << '\n';
                top.md_trace.diagnostic();
                auto* buf = top.cache_sub.get_pt_dedup_buffer();
                if (buf) {
                    std::cerr << "[REAL16_STATE] dedup_buffer_valid=" << buf->get_valid_count()
                              << " dedup_req_lines=" << top.cache_sub.get_dedup_req_line_count()
                              << " prefetch_groups=" << top.prefetch_groups.size()
                              << " ptw_active=" << top.ptw_active_walks.size()
                              << " live_pairs=" << page_table.size()
                              << " pool_used=" << pool_next << '\n';
                }
                require(false, "16盘回放超时: 见 [REAL16_TIMEOUT] 分类(功能错误与超时已区分)");
            }
        }
    }

    // ---------------------------- 主流程 ----------------------------
    void run() {
        wait(20, SC_NS);
        start_ns = iommu_md::now_ns();

        require(N == 16, "16盘回放要求 TEST_CFG_NUM_DEVICES=16");

        const char* rd  = std::getenv("REAL16_READ_CSV");
        const char* rd1 = std::getenv("REAL16_READ_CSV1");
        const char* wr  = std::getenv("REAL16_WRITE_CSV");
        const char* wr1 = std::getenv("REAL16_WRITE_CSV1");
        merge_group(0, rd ? rd : "real16_input/read_trace.csv",
                       rd1 ? rd1 : "real16_input/read_trace_1.csv");
        merge_group(1, wr ? wr : "real16_input/write_trace.csv",
                       wr1 ? wr1 : "real16_input/write_trace_1.csv");
        total_requests = stats[0].rows + stats[1].rows;

        // 启动自检: 打印编译宏与实际生效配置, 不依赖文件名或旧日志推断。
        std::cout << "[REAL16_CONFIG] N=" << N << " groups=2(split@DID" << GROUP_SPLIT
                  << ") IOVA_MB=" << RANGE_MB
                  << " pool_pages=" << POOL_PAGES << " pool_seed=" << PAGE_POOL_SEED
                  << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " ptw=" << PTW_MAX_OUTSTANDING_TASKS
                  << " buffer=" << PT_DEDUP_BUFFER_SIZE
                  << " G/R/W=" << IOMMU_GLOBAL_MAX_OUTSTANDING << '/'
                  << IOMMU_READ_MAX_OUTSTANDING << '/' << IOMMU_WRITE_MAX_OUTSTANDING
                  << " adm=" << TEST_CFG_DEDUP_ADMISSION_CTRL
                  << " split=" << TEST_CFG_SPLIT_RW_PORTS
                  << " perdev_wo=" << TEST_CFG_PERDEV_WRITE_ORDER
                  << " port_bit=" << AXI_SLAVE_0_WIDTH_BIT
                  << " slink_ns=" << SLINK_NOC_LATENCY_NS
                  << " ddr_ns=" << DDR_Module::DDR_INTERNAL_LATENCY_NS << '\n';
        std::cout << "[REAL16_TOTAL] read=" << stats[0].rows << " write=" << stats[1].rows
                  << " all=" << total_requests
                  << " bytes=" << (stats[0].bytes + stats[1].bytes)
                  << " data=" << (stats[0].data_rows + stats[1].data_rows)
                  << " ctrl=" << (stats[0].ctrl_rows + stats[1].ctrl_rows) << '\n';
        for (unsigned d = 0; d < 2; ++d) {
            std::cout << "[REAL16_DEV] dir=" << (d ? "W" : "R");
            for (unsigned i = 0; i < N; ++i) std::cout << " did" << i << "=" << stats[d].per_dev_rows[i];
            std::cout << '\n';
        }
        fflush(stdout);

        setup();

        payloads.reserve(total_requests);
        payload_exts.reserve(total_requests);
        pair_log.reserve(total_requests / 4 + 1024);

        remap_out.reset(new std::ofstream("real16_trace_remap.csv"));
        require(remap_out->good(), "无法创建 real16_trace_remap.csv");
        *remap_out << "dir,group,excel_row,did,bytes,is_ctrl,source_page_no,offset,gen_id,"
                      "mapped_page_no,gpa_page_base,expected_spa\n";

        // 冷启动: 无哨兵/预热, 直接进入正式回放; 统计窗口在此重置。
        top.reset_4kb_concurrency_window();
        top.cache_sub.reset_dedup_valid_line_stats();
        // [计划] RP以同一实际N设置模型内稳态阈值(#floor(N*10%)~#floor(N*90%)),
        //   供模型稳态窗口统计(Steady IOPS/带宽/端口稳态间隔)与离线 W 逐项对账。
        top.steady_start_count = (uint64_t)total_requests * 10 / 100;
        top.steady_end_count   = (uint64_t)total_requests * 90 / 100;
        std::cout << "[REAL16_STAT_WINDOW] N=" << total_requests
                  << " steady_start_count=" << top.steady_start_count
                  << " steady_end_count=" << top.steady_end_count << '\n';
        top.open_real8_dedup_occupancy_trace("real16_dedup_occupancy.csv");
        top.open_real8_ptw_occupancy_trace("real16_ptw_occupancy.csv");
        top.md_trace.set_operations_path("real16_operations_raw.csv");

        sc_spawn([this]() { sender(0); });
        sc_spawn([this]() { sender(1); });
        sc_spawn([this]() { watchdog(); });

        while (true) {
            if (send_done[0] && send_done[1] && total_responded == total_requests &&
                top.md_quiescent()) {
                break;
            }
            wait(1, SC_NS);
        }
        stopped = true;

        if (remap_out) { remap_out->flush(); remap_out->close(); remap_out.reset(); }
        collect_residuals();
        finish();
    }

    void finish() {
        // 占用采样流先关闭, 再输出报告(避免收尾统计写盘与占用流交错)。
        top.close_real8_dedup_occupancy_trace();
        top.close_real8_ptw_occupancy_trace();
        // 配置回读: 校验计划要求的 Cache 几何与并发额度确实生效。
        //   num_sets()语义=全体 set 数(4 RAM x 16 set/RAM = 64);
        //   num_ways()=4; total_lines()=4 RAM x 64 = 256(计划固定值)。
        std::cout << "[REAL16_EFFECTIVE] dedup_sets_total=" << top.cache_sub.get_dedup_num_sets()
                  << " dedup_ways=" << top.cache_sub.get_dedup_num_ways()
                  << " dedup_lines=" << top.cache_sub.get_dedup_total_lines()
                  << " buffer=" << PT_DEDUP_BUFFER_SIZE
                  << " ptw_cap=" << PTW_MAX_OUTSTANDING_TASKS
                  << " ctrl_total=" << top.ctrl_total_count
                  << " kb4_in=" << top.kb4_dev_stats.size() << '\n';
        require(top.cache_sub.get_dedup_num_sets() == 64 &&
                top.cache_sub.get_dedup_num_ways() == 4 &&
                top.cache_sub.get_dedup_total_lines() == 256,
                "去重Cache 几何与计划不符(期望 4 RAM x 16 set/RAM x 4 way = 256, 4 个RAM)");
        require(PT_DEDUP_BUFFER_SIZE == 320, "去重 Buffer 容量与计划不符(期望 320)");
        require(PTW_MAX_OUTSTANDING_TASKS == 64, "PTW 并发与计划不符(期望 64)");
        require(IOMMU_GLOBAL_MAX_OUTSTANDING == 640 && IOMMU_READ_MAX_OUTSTANDING == 320 &&
                IOMMU_WRITE_MAX_OUTSTANDING == 320, "并发额度与计划不符(期望 640/320/320)");

        const std::string prefix = "real16_trace";
        top.md_trace.report(prefix);
        write_pairs(prefix);
        // [16盘实测回放] Buffer 全期占用/持有统计(旧场景同款口径, 仅本场景文件内打印):
        //   W 内口径由离线脚本从占用流与 operations 区间重算, 此处为全期对照。
        auto* dedup_buf = top.cache_sub.get_pt_dedup_buffer();
        if (dedup_buf) {
            printf("\n========== PT Dedup Buffer Statistics ==========\n");
            printf("  Buffer Size:        %u entries\n", PT_DEDUP_BUFFER_SIZE);
            printf("  Peak Valid Count:   %u entries\n", dedup_buf->get_peak_valid_count());
            printf("  Current Valid:      %u entries\n", dedup_buf->get_valid_count());
            printf("  Peak Usage:         %.1f%%\n", 100.0 * dedup_buf->get_peak_valid_count() / PT_DEDUP_BUFFER_SIZE);
            printf("  Buffer Full Bypass: %lu tasks\n", (unsigned long)top.cache_sub.get_dedup_buffer_full_bypass_count());
            printf("  --- Buffer 实际占用率 (时间加权平均) ---\n");
            printf("    Avg Occupancy:    %.2f entries / %u (%.2f%%)\n",
                   dedup_buf->get_avg_occupancy(), PT_DEDUP_BUFFER_SIZE, dedup_buf->get_avg_occupancy_pct());
            printf("    Occupancy window: %.1f ns\n", dedup_buf->get_occupancy_window_ns());
            printf("  --- Buffer Task Hold Time (alloc -> PTW-done free) ---\n");
            printf("    Avg hold:  %.1f ns\n", dedup_buf->get_avg_hold_ns());
            printf("    Max hold:  %.1f ns\n", dedup_buf->get_max_hold_ns());
            printf("    Min hold:  %.1f ns\n", dedup_buf->get_min_hold_ns());
            printf("    Samples:   %lu\n", (unsigned long)dedup_buf->get_hold_count());
            printf("==================================================\n");
        }
        if (top.md_hold_main_cnt || top.md_hold_susp_cnt || top.md_hold_pf_cnt) {
            printf("\n========== Dedup Buffer Hold Decomposition ==========\n");
            printf("  总持有延时 = W1 + W2 + W3;  W1=alloc->group_complete,\n");
            printf("  W2=group_complete->agg_flush_exec, W3=agg_flush_exec->entry_free\n");
            auto pr_hold = [](const char* label, uint64_t n, const double* sum, const double* mx) {
                printf("  %-30s n=%-7lu avg total=%8.1f ns (W1=%7.1f W2=%6.1f W3=%5.1f) | maxSeg %.1f/%.1f/%.1f\n",
                       label, (unsigned long)n,
                       n ? (sum[0]+sum[1]+sum[2])/(double)n : 0.0,
                       n ? sum[0]/(double)n : 0.0, n ? sum[1]/(double)n : 0.0, n ? sum[2]/(double)n : 0.0,
                       mx[0], mx[1], mx[2]);
            };
            pr_hold("MAIN head (dedup MISS)", top.md_hold_main_cnt, top.md_hold_main_sum, top.md_hold_main_max);
            pr_hold("MAIN head (PF placeholder)",  top.md_hold_pf_cnt,  top.md_hold_pf_sum,  top.md_hold_pf_max);
            pr_hold("SUSPENDED (deduped)",         top.md_hold_susp_cnt, top.md_hold_susp_sum, top.md_hold_susp_max);
            printf("  PTW prefetch tasks holding buffer entries: 0\n");
            printf("    (预取任务不申请去重Buffer entry, 只插 dedup Cache 的 is_req=0 占位行并占 PTW 槽位)\n");
            printf("======================================================\n");
        }
        top.print_cache_statistics();
        top.print_4kb_concurrency_report();

        std::cout << "[REAL16_RESULT] PASS expected=" << total_requests
                  << " completed=" << total_responded
                  << " resp_r=" << resp[0] << " resp_w=" << resp[1]
                  << " bytes_r=" << resp_bytes[0] << " bytes_w=" << resp_bytes[1]
                  << " gen_created=" << gen_created
                  << " gen_completed=" << gen_completed
                  << " pool_used=" << pool_next
                  << " residual_pairs=" << (pair_log.size() - gen_completed)
                  << " timeout_diag=" << timeout_diag << '\n';
        fflush(stdout);
        require(total_responded == total_requests, "响应总数与请求总数不一致");
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario = std::make_shared<Real16TraceScenario>(*this);
    md_response_handler = [scenario](tlm_generic_payload& trans) { scenario->response(trans); };
    scenario->run();
}
#endif
