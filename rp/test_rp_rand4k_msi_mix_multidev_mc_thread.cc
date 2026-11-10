#include "test_rp.hh"
#include "iommu_top.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include <array>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <numeric>

// ============================================================================
// 多设备 × 每设备多通道 激励注入场景 (Multi-Device × Multi-Channel Scenario)
//
// 目标: 在场景13-v4 多设备(N)基线之上, 把"每设备 1 条顺序流"扩展为"每设备内部
//       C 条并发通道", 共 S=N*C 条独立注入流, 经"通道外层/设备内层"的 2D 逐包
//       轮询(i = c*N + d)交错注入 -> 各通道 4KB 的 8x512B 高度交织。
//
// 关键设计(融合 multidev 的设备/页表/全局唯一页 + multichan 的通道/producer/RR):
//   - S 个 producer 进程: 各自把本流 Request 压入自有有界队列(反压), 不调用入口;
//   - 1 个 RR scheduler: 唯一 nb_transport_fw 调用者, 逐包轮询(每次取1个Request),
//     游标 next 每次 +1 并对 S 取模, 顺序 i=c*N+d (设备 d 内层、通道 c 外层);
//   - 1 个 watchdog: 沿用多设备超时诊断;
//   - 统计按设备(md_trace, did=0..N-1)聚合, 通道对 trace 透明。
//
// 负载口径 fixed_total: 每设备总量 GROUPS 组, 拆分到 C 个通道(chan_groups(c)),
//   每设备包数 = GROUPS*11 + 1 = 10000(tail MSI 只给通道0), 总 = N*10000。
// 页策略: 全局唯一随机页, N*GROUPS 个 4KB 页两两不重复(跨设备跨通道), 选页上界
//   PAGE_PICK_RANGE 预留 PT_DEDUP_PREFETCH_DEPTH 个 headroom, 保证预取邻居均被映射。
//
// 本文件仅在 TEST=rand4k_msi_mix_multidev_mc 时经 TEST_THREAD_SRC 编入; 其余场景不编译。
// 复用多设备追踪(md_trace), 需 Makefile 同时置 TEST_CFG_MULTI_DEVICE_SCENE=1。
// ============================================================================

// ---- 场景编译宏安全默认(Makefile 正常注入; 此处仅兜底, 不改共享头) ----
#ifndef TEST_CFG_MULTIDEV_MC_SCENE
#define TEST_CFG_MULTIDEV_MC_SCENE 0
#endif
#ifndef TEST_CFG_MD_CHAN_PER_DEV
#define TEST_CFG_MD_CHAN_PER_DEV 8
#endif
#ifndef TEST_CFG_NUM_DEVICES
#define TEST_CFG_NUM_DEVICES 16
#endif
#ifndef TEST_CFG_MD_QUEUE_DEPTH
#define TEST_CFG_MD_QUEUE_DEPTH 32
#endif
#ifndef TEST_CFG_MD_RR_START
#define TEST_CFG_MD_RR_START 0
#endif
#ifndef TEST_CFG_NUM_PAGES
#define TEST_CFG_NUM_PAGES 909
#endif
#ifndef TEST_CFG_MC_RW_QUEUE_DEPTH       // [读写端口分离] 读/写发送队列深度(近乎无界)
#define TEST_CFG_MC_RW_QUEUE_DEPTH 65536
#endif
#ifndef TEST_CFG_MD_DATA_ONLY           // [纯数据负载] 1=测量仅 8x512B 读写数据, 不发 SQ/CQ/MSI 控制包
#define TEST_CFG_MD_DATA_ONLY 0
#endif
#ifndef TEST_CFG_MD_NVME10              // 仅新测试项启用：8数据+SQ64B+CQ16B，不发送MSI。
#define TEST_CFG_MD_NVME10 0
#endif

// 复用多设备追踪(md_trace/md_response_handler), 必须同时开启 MULTI_DEVICE_SCENE。
#if TEST_CFG_MULTIDEV_MC_SCENE && !TEST_CFG_MULTI_DEVICE_SCENE
#error "TEST_CFG_MULTIDEV_MC_SCENE 需要 TEST_CFG_MULTI_DEVICE_SCENE=1 (复用 md_trace)"
#endif

#if TEST_CFG_MULTIDEV_MC_SCENE
namespace {
using iommu_md::require;
constexpr unsigned N = TEST_CFG_NUM_DEVICES;                 // 设备数
constexpr unsigned C = TEST_CFG_MD_CHAN_PER_DEV;             // 每设备通道数
constexpr unsigned S = N * C;                                // 总流数(=producer数)
constexpr unsigned QUEUE_DEPTH = TEST_CFG_MD_QUEUE_DEPTH;
constexpr uint64_t RWQ_DEPTH = TEST_CFG_MC_RW_QUEUE_DEPTH;   // [读写端口分离] 发送队列深度上限(近乎无界, 仅记录峰值)
constexpr unsigned RR_START = TEST_CFG_MD_RR_START;
constexpr unsigned GROUPS = TEST_CFG_NUM_PAGES;              // 每设备总组数(fixed_total)
constexpr bool NVME10 = TEST_CFG_MD_NVME10 != 0;
constexpr unsigned GROUP_PACKETS = NVME10 ? 10u : 11u;
constexpr unsigned SQ_BYTES = NVME10 ? 64u : 32u;
static_assert(TEST_CFG_MD_NVME10 == 0 || TEST_CFG_MD_NVME10 == 1, "NVME10必须为0或1");
static_assert(!NVME10 || !TEST_CFG_MD_DATA_ONLY, "十包组与纯数据模式互斥");
// fixed_total: 每设备 GROUPS 组均分到 C 个通道, 余数分给前 GROUPS_REM 个通道。
constexpr unsigned GROUPS_BASE = GROUPS / C;
constexpr unsigned GROUPS_REM  = GROUPS % C;
constexpr uint64_t IOVA_BASE = 0x100000ULL;
#ifdef TEST_CFG_S13_IOVA_512MB
constexpr unsigned RANGE_MB = 512, HUGE_COUNT = 50;
constexpr uint64_t MSI_PATTERN = 0x7000, MSI_IOVA_BASE = 0x21000000;
constexpr uint64_t SQ_IOVA = 0x22000000, CQ_IOVA = 0x22001000, CTRL_GPA = 0x8000000;
#else
constexpr unsigned RANGE_MB = 16, HUGE_COUNT = 20;
constexpr uint64_t MSI_PATTERN = 0x3000, MSI_IOVA_BASE = 0x2000000;
constexpr uint64_t SQ_IOVA = 0x5000000, CQ_IOVA = 0x5001000, CTRL_GPA = 0x4000000;
#endif
constexpr unsigned RANGE_PAGES = RANGE_MB * 256;
// [边界安全] 全局唯一选页上界: 预留 PT_DEDUP_PREFETCH_DEPTH 个页, 使任何被选页的
//   预取邻居 page+k(k<=D) 都 < RANGE_PAGES, 与 RP 建表 mapped 集合一致, 杜绝边界无效 PTE。
constexpr unsigned PAGE_PICK_RANGE = RANGE_PAGES - PT_DEDUP_PREFETCH_DEPTH;
constexpr uint64_t PROBE_IOVA = SQ_IOVA + 0x10000;
constexpr uint64_t HUGE = 0x200000ULL;

static_assert(N >= 1 && N <= 16, "设备数必须为1..16");
static_assert(C >= 1 && C <= 16, "每设备通道数必须为1..16");
static_assert(S >= 1 && S <= 256, "总流数(N*C)必须为1..256");
static_assert(QUEUE_DEPTH > 0 && QUEUE_DEPTH <= 1048576, "就绪队列深度非法");
static_assert(GROUPS > 0 && GROUPS <= RANGE_PAGES, "每设备组数超过候选页数");
static_assert(GROUPS >= C, "fixed_total 要求每设备组数不少于通道数");
static_assert(uint64_t(GROUPS) * N <= PAGE_PICK_RANGE,
              "全局唯一模式要求 N*GROUPS 不超过可选页数(PAGE_PICK_RANGE)");
static_assert(PT_DEDUP_PREFETCH_DEPTH <= 16, "预取深度超过结果槽容量");
static_assert(RR_START < S, "轮询起点非法");

// 流号 i 的 (设备, 通道): i = c*N + d -> d = i%N(内层快变), c = i/N(外层慢变)。
constexpr unsigned dev_of(unsigned i)  { return i % N; }
constexpr unsigned chan_of(unsigned i) { return i / N; }
// 每通道组数/tail数: 前 GROUPS_REM 个通道各多1组; tail MSI 仅通道0, 使每设备精确 GROUPS*11+1。
constexpr unsigned chan_groups(unsigned c) { return GROUPS_BASE + (c < GROUPS_REM ? 1u : 0u); }
constexpr unsigned chan_tails(unsigned c)  { return !NVME10 && c == 0u ? 1u : 0u; }
// 每流各 epoch 的包数: 预热/哨兵只由每设备通道0发起(设备级共享 dc/SQ/CQ/MSI/probe)。
constexpr uint64_t stream_count(unsigned i, unsigned e) {
    const unsigned c = chan_of(i);
    if (e == 1) return c == 0u ? 2u : 0u;    // 哨兵: 打通翻译/响应路由。
    if (TEST_CFG_MD_DATA_ONLY)               // [纯数据负载] 无控制包预热; 测量每设备=组数x8个512B数据包
        return (e == 2) ? 0u : uint64_t(chan_groups(c)) * 8u;
    if (e == 2) return c == 0u ? 16u : 0u;   // 预热: MSI(x8)+SQ/CQ 交替。
    return uint64_t(chan_groups(c)) * GROUP_PACKETS + chan_tails(c);
}
constexpr uint64_t total_count(unsigned e) {
    uint64_t s = 0;
    for (unsigned i = 0; i < S; ++i) s += stream_count(i, e);
    return s;
}

uint64_t spa_offset(unsigned d) { return 0x80000000ULL + uint64_t(d) * 0x10000000ULL; }
uint64_t msi_pa(unsigned d, unsigned v) { return 0x40000000ULL + uint64_t(d)*0x100000ULL + uint64_t(v)*4096 + 0x40; }

// 设备级共享状态(上下文/VS映射/MSI/控制包), 该设备 C 个通道全部复用。
struct Device {
    device_context_t dc{};
    uint64_t dc_address = 0, msi_iova = 0, probe_gpa = 0;
    std::vector<unsigned> pages, msi_pages;   // pages: 本设备 GROUPS 个全局唯一页
    std::vector<uint64_t> gpas;               // 共享 VS 映射: gpas[RANGE_PAGES]
};
// 单条注入流(设备 d 的通道 c): 独立随机页子集 + 独立有界就绪队列。
struct Stream {
    unsigned dev = 0, chan = 0;
    std::vector<unsigned> pages;              // 本通道 chan_groups(c) 个页(dev.pages 的切片)
    std::deque<iommu_md::Request> queue;
    sc_event space_event;
    bool ready = false, done = false;
    uint64_t injected = 0, scheduled = 0, peak_queue = 0, full_waits = 0;
    double full_wait_ns = 0;
};

// 所有SystemC进程属于单个RP，只有scheduler访问唯一的initiator socket。
class MultiDeviceMcScenario {
    RP_Module& rp;
    iommu_top& top;
    std::array<Device, N> devices;
    std::array<Stream, S> streams;
    uint64_t total_responded = 0;
    std::vector<std::unique_ptr<tlm_generic_payload>> payloads;
    sc_event phase_event, ready_event;
    unsigned epoch = 0;
    bool sent = false, stopped = false;
    double start_ns = 0;
    uint64_t last_progress = 0;
    double last_progress_ns = 0;

#if TEST_CFG_SPLIT_RW_PORTS
    // [读写端口分离] scheduler 按 RR 选出的请求按方向压入读/写发送队列, 由 sender_r/sender_w
    //   各自调用对应端口 socket 的 nb_transport_fw 发送。回调内的带宽延时+并发门控自然阻塞
    //   本方向发送进程, 读写互不影响; 写方向单一 FIFO 天然保持每设备写注入顺序(写保序)。
    struct SendItem {
        tlm_generic_payload* ptr;      // 已 register_request 的 payload
        iommu_md::Request    record;   // 请求副本(用于发送后打印/计数)
        Stream*              strm;     // 所属流(++injected)
    };
    std::deque<SendItem> rw_queue_r, rw_queue_w;
    sc_event rw_event_r, rw_event_w;
    bool sender_busy_r = false, sender_busy_w = false;   // sender 正在 nb_transport_fw 内阻塞
    uint64_t rw_peak_r = 0, rw_peak_w = 0;               // 两队列峰值占用(报告用)
    // [供给验证] 发送侧饥饿统计: sender 因本方向队列为空而等待的次数/时长。
    uint64_t starve_cnt_r = 0, starve_cnt_w = 0;
    double   starve_ns_r = 0.0, starve_ns_w = 0.0;
    double   last_send_ns_r = 0.0, last_send_ns_w = 0.0;  // 各口最后一次送出请求的时刻
    bool senders_idle() const { return !sender_busy_r && !sender_busy_w; }
#endif

    static std::vector<unsigned> choose(unsigned count, unsigned range, unsigned seed, unsigned shuffle_seed) {
        std::mt19937 rng(seed), permutation(shuffle_seed);
        std::set<unsigned> chosen;
        while (chosen.size() < count) chosen.insert(rng() % range);
        std::vector<unsigned> result(chosen.begin(), chosen.end());
        for (size_t i=result.size();i>1;--i) std::swap(result[i-1],result[permutation()%i]);
        return result;
    }
    void map_vs(Device& dev, uint64_t iova, uint64_t gpa) {
        spte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=gpa/4096;
        require(rp.add_vs_stage_pte(&top,dev.dc.fsc.iosatp,iova,p,0,dev.dc.iohgatp,0)!=uint64_t(-1),
                "构建VS页表失败");
    }
    void map_g(Device& dev, uint64_t gpa, uint64_t spa, unsigned level) {
        gpte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=spa/4096;
        require(rp.add_g_stage_pte(&top,dev.dc.iohgatp,gpa,p,level)!=uint64_t(-1), "构建G-stage失败");
    }
    void setup() {
        // 保留现有低地址寄存器/CQ区域，只初始化一次物理分配游标。
        next_free_page=std::max<uint64_t>(next_free_page,240);
        // 最坏页表预算：每个2MiB IOVA区一张L0表，另留目录、G-stage及MSI页裕量。
        const uint64_t per_device_pages=(uint64_t(RANGE_MB)+2)/2+64;
        require((next_free_page+1+N*per_device_pages)*4096 <= DDR_Module::DDR_MEMORY_SIZE,
                "所有设备页表预算超过32MiB DDR");
        require(rp.enable_iommu(&top,DDT_1LVL)>=0,"启用IOMMU失败");

        // [全局唯一独立页] 一次性从整个 IOVA 范围抽取 N*GROUPS 个互不重复的页号,
        //   先按设备切片(每设备 GROUPS 个), 再在设备内按 chan_groups(c) 切给各通道,
        //   使 N*C 条流的页集合两两不相交 -> "N*GROUPS 个 4KB 页全不重复"。
        std::vector<unsigned> all_pages = choose(N*GROUPS, PAGE_PICK_RANGE, 2024, 123);

        for(unsigned d=0;d<N;++d) {
            auto& dev=devices[d];
            dev.pages.assign(all_pages.begin()+d*GROUPS, all_pages.begin()+(d+1)*GROUPS);
            dev.msi_pages=choose(256,1024,777+d,777+d);
            dev.msi_iova=MSI_IOVA_BASE+uint64_t(dev.msi_pages[0])*4096+0x40;
            dev.probe_gpa=0x100000ULL+uint64_t(d)*4096;
            next_free_gpage[d+1]=0x10000;
#if TEST_CFG_MD_NVME10
            // [建表统一 20261103] 与 real8 同构: MSIPTP_Off(nvme10 模式不合成 MSI),
            //   消除 MSI 表页与 G-stage 页表页的分配交叠嫌疑(回归门: hugepage LEAF>0)。
            dev.dc_address=rp.add_device(&top,d,d+1,0,0,0,0,0,1,1,0,0,0,
                                        IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Off,0,0,0);
#else
            dev.dc_address=rp.add_device(&top,d,d+1,0,0,0,0,0,1,1,0,0,0,
                                        IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Flat,1,0xFF,MSI_PATTERN);
#endif
            require(dev.dc_address!=uint64_t(-1),"添加设备失败");
            rp.read_memory_test_rp(dev.dc_address,sizeof(dev.dc),reinterpret_cast<char*>(&dev.dc));
            dev.dc.ta.PSCID=0;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&dev.dc),dev.dc_address,sizeof(dev.dc));
            device_context_t readback{};
            rp.read_memory_test_rp(dev.dc_address,sizeof(readback),reinterpret_cast<char*>(&readback));
            require(readback.iohgatp.GSCID==d+1 && readback.ta.PSCID==0 && readback.tc.PDTV==0,
                    "DC读回上下文不匹配");
#if !TEST_CFG_MD_NVME10
            for(unsigned h=0;h<HUGE_COUNT;++h) map_g(dev,uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(d),1);
            for(unsigned v=0;v<256;++v) {
                msipte_t mp{};
                mp.V=1; mp.M=3; mp.translate_rw.PPN=msi_pa(d,v)/4096;
                rp.write_memory_test_rp(reinterpret_cast<char*>(&mp),dev.dc.msiptp.PPN*4096+v*16,16);
                map_vs(dev,MSI_IOVA_BASE+uint64_t(dev.msi_pages[v])*4096,(MSI_PATTERN<<12)+uint64_t(v)*4096);
            }
#endif
            dev.gpas.resize(RANGE_PAGES);
            std::mt19937 mapping(42+d);
            std::array<unsigned,HUGE_COUNT> slots{};
            // 先生成全范围映射值(每设备种子不同 -> 各设备 S1/S2 结果不同)。
            for(unsigned i=0;i<RANGE_PAGES;++i) {
                unsigned h=mapping()%HUGE_COUNT, slot=0;
                if(RANGE_MB==16) {
                    while(slots[h]>=512) h=(h+1)%HUGE_COUNT;
                    slot=slots[h]++;
                } else slot=mapping()%512;
                dev.gpas[i]=uint64_t(h)*HUGE+uint64_t(slot)*4096;
            }
#if TEST_CFG_MD_NVME10
            // [建表统一 20261103] 与 real8 同序: gpas -> GS 50×2MiB 大页叶 -> VS。
            for(unsigned h=0;h<HUGE_COUNT;++h) map_g(dev,uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(d),1);
#endif
            // VS 数据映射: nvme10 统一为全范围预建(与 real8 同构, 覆盖全部预取邻居);
            // 其他模式保持在用页 ∪ D 邻居。
            std::set<unsigned> mapped;
            if(RANGE_MB==16 || TEST_CFG_MD_NVME10) for(unsigned i=0;i<RANGE_PAGES;++i) mapped.insert(i);
            else for(unsigned page:dev.pages) {
                unsigned vpn0=((IOVA_BASE>>12)+page)&511;
                unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-vpn0);
                for(unsigned k=0;k<=depth && page+k<RANGE_PAGES;++k) mapped.insert(page+k);
            }
            for(unsigned page:mapped) map_vs(dev,IOVA_BASE+uint64_t(page)*4096,dev.gpas[page]);
            map_vs(dev,SQ_IOVA,CTRL_GPA);
            map_vs(dev,CQ_IOVA,CTRL_GPA+4096);
            map_g(dev,CTRL_GPA,CTRL_GPA+spa_offset(d),0);
            map_g(dev,CTRL_GPA+4096,CTRL_GPA+4096+spa_offset(d),0);
            // 独立哨兵使各设备S1结果也不同；不计入正式性能请求。
            map_vs(dev,PROBE_IOVA,dev.probe_gpa);
            for(uint64_t base:{SQ_IOVA,CQ_IOVA,PROBE_IOVA}) {
                unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-((base>>12)&511));
                for(unsigned k=1;k<=depth;++k) {
                    uint64_t page=base+uint64_t(k)*4096;
                    if(page==SQ_IOVA || page==CQ_IOVA || page==PROBE_IOVA) continue;
                    map_vs(dev,page,0x200000ULL+uint64_t(k)*4096);
                }
            }
            // 设备内把 GROUPS 页按 chan_groups(c) 顺序切给 C 个通道(流号 i=c*N+d)。
            unsigned base_c=0;
            for(unsigned c=0;c<C;++c) {
                auto& strm=streams[c*N+d];
                strm.dev=d; strm.chan=c;
                strm.pages.assign(dev.pages.begin()+base_c, dev.pages.begin()+base_c+chan_groups(c));
                base_c+=chan_groups(c);
            }
            require(base_c==GROUPS,"通道页切片总数与每设备组数不一致");
            uint64_t digest=1469598103934665603ULL;
            for(unsigned page:dev.pages) digest=(digest^page)*1099511628211ULL;
            std::cout << "[MDMC_SETUP] DID=" << d << " GSCID=" << d+1 << " PSCID=0 chan=" << C
                      << " pages=" << mapped.size() << " groups=" << GROUPS
                      << " sequence_hash=" << digest << " root=" << dev.dc.iohgatp.PPN
                      << " free_ppn=" << next_free_page << '\n';
            top.md_trace.progress();
        }
        // [全局唯一校验] 汇总所有 N*C 条流页号, 断言两两不重复且总数=N*GROUPS。
        {
            std::set<unsigned> uniq; unsigned total=0;
            for(unsigned i=0;i<S;++i) {
                uniq.insert(streams[i].pages.begin(),streams[i].pages.end());
                total+=streams[i].pages.size();
            }
            require(uniq.size()==total && total==N*GROUPS,
                    "全局唯一页校验失败: 存在跨设备/跨通道重复页");
            std::cout << "[MDMC_UNIQUE_OK] devices=" << N << " chan/dev=" << C
                      << " streams=" << S << " groups/dev=" << GROUPS
                      << " total_unique=" << uniq.size() << " expected=" << N*GROUPS << '\n';
        }
#if TEST_CFG_MD_NVME10
        // [建表统一 20261103] 与 real8 同构: 建表完成后统一全局失效一次 + 页表指纹。
        invalidate_all();
        std::cout << "[MDMC_TB_FINGERPRINT] msiptp=Off huge_leaves=" << (unsigned)HUGE_COUNT * N
                  << " vs_policy=full_range order=gpas->GS_huge->VS->ctrl/probe->inval" << '\n';
#endif
    }
    void invalidate_all() {
        iommu::CacheMessage cmd;
        cmd.msg_type=iommu::CacheMsgType::DC_INVALIDATE;
        cmd.cmd_type=iommu::InvalidCmdType::GLOBAL_INVAL;
        cmd.invalidate_mode=iommu::CacheInvalidateMode::GLOBAL;
        top.cache_sub.invalidation_request_fifo.write(cmd);
        auto response=top.cache_sub.invalidation_response_fifo.read();
        require(response.msg_type==iommu::CacheMsgType::CACHE_INVALIDATE_RESPONSE,"全局失效未完成");
        top.md_trace.progress();
    }
    iommu_md::Request descriptor(unsigned i,unsigned e,uint64_t seq) {
        auto& strm=streams[i];
        auto& dev=devices[strm.dev];
        const unsigned d=strm.dev, c=strm.chan;
        iommu_md::Request r;
        r.device=d; r.epoch=e; r.sequence=seq;
        if(e==1) {   // 哨兵: 同 IOVA、每设备不同 S1/S2 结果。
            r.bytes=512; r.iova=PROBE_IOVA; r.expected_pa=dev.probe_gpa+spa_offset(d);
            return r;
        }
        unsigned type;
        if(e==2 && NVME10) type=seq%2 ? 2 : 1;      // 十包模式只预热SQ/CQ，不发送MSI。
        else if(e==2) type=seq<8 ? 3 : (seq%2 ? 2 : 1);   // 预热: MSI(x8)+SQ/CQ 交替。
        else if(TEST_CFG_MD_DATA_ONLY) {             // [纯数据负载] 每组仅 8x512B 数据, 读写按组号奇读偶写交替
            r.group=seq/8;
            unsigned offset=seq%8;
            type=0;
            unsigned page=strm.pages[r.group];
            r.iova=IOVA_BASE+uint64_t(page)*4096+offset*512;
            r.expected_pa=dev.gpas[page]+spa_offset(d)+offset*512;
            r.bytes=512; r.write=(r.group%2)!=0;
        }
        else {
            unsigned g_local=seq/GROUP_PACKETS;
            // [组号通道唯一 20261103] C>1 时同设备不同通道的局部组号在 (did,group)
            //   统计键上冲突; record.group 改用通道唯一编号 c*GROUPS+g_local
            //   (C=1 时退化为原值); 页取址与读写奇偶仍用通道内局部组号, 激励不变。
            r.group=uint64_t(c)*GROUPS+g_local;
            unsigned offset=seq%GROUP_PACKETS;
            type=seq>=uint64_t(chan_groups(c))*GROUP_PACKETS ? 3 : offset<8 ? 0 : offset-7;
            if(type==0) {
                unsigned page=strm.pages[g_local];
                r.iova=IOVA_BASE+uint64_t(page)*4096+offset*512;
                r.expected_pa=dev.gpas[page]+spa_offset(d)+offset*512;
                r.bytes=512; r.write=(g_local%2)!=0;   // 奇读偶写交替(通道内组号%2)。
            }
        }
        r.type=type;
        if(type==1) { r.iova=SQ_IOVA; r.expected_pa=CTRL_GPA+spa_offset(d); r.bytes=SQ_BYTES; }
        if(type==2) { r.iova=CQ_IOVA; r.expected_pa=CTRL_GPA+4096+spa_offset(d); r.bytes=16; r.write=true; }
        if(type==3) { r.iova=dev.msi_iova; r.expected_pa=msi_pa(d,0); r.bytes=4; r.write=true; }
        return r;
    }
    // 只校验描述符，不访问端口、不推进仿真时间；旧模式完全跳过。
    void validate_nvme10() {
        if (!NVME10) return;
        uint64_t bytes=0, data=0, sq=0, cq=0;
        for (unsigned i=0; i<S; ++i) {
            require(stream_count(i,3)==uint64_t(chan_groups(chan_of(i)))*10,
                    "十包模式出现额外尾包");
            for (unsigned g=0; g<chan_groups(chan_of(i)); ++g) {
                uint64_t page=0;
                for (unsigned k=0; k<10; ++k) {
                    auto r=descriptor(i,3,uint64_t(g)*10+k);
                    require(r.device==dev_of(i) && r.group==uint64_t(chan_of(i))*GROUPS+g, "十包组设备/组号错误");
                    require(r.type==(k<8 ? 0u : k-7), "十包组包型或顺序错误");
                    require(r.bytes==(k<8 ? 512u : k==8 ? 64u : 16u), "十包组长度错误");
                    if (k<8) {
                        if (k==0) page=r.iova;
                        require(r.iova==page+k*512 && (page&4095)==0,
                                "数据组不是同一4KB页内的连续8个512B");
                        require(r.write==((g%2)!=0), "数据组读写方向错误");
                        ++data;
                    } else if (k==8) {
                        require(!r.write, "SQ必须为读请求"); ++sq;
                    } else {
                        require(r.write, "CQ必须为写请求"); ++cq;
                    }
                    bytes+=r.bytes;
                }
            }
            for (uint64_t s=0; s<stream_count(i,2); ++s)
                require(descriptor(i,2,s).type!=3, "预热阶段禁止发送MSI");
        }
        require(data==uint64_t(N)*GROUPS*8 && sq==uint64_t(N)*GROUPS && cq==sq,
                "十包组包数不闭合");
        require(bytes==uint64_t(N)*GROUPS*4176, "十包组字节总数不闭合");
        std::cout << "[NVME10_PATTERN_OK] data512=" << data << " sq64=" << sq
                  << " cq16=" << cq << " msi=0 tail=0 bytes=" << bytes
                  << " avg_bytes=417.6 order=DATA8,SQ,CQ msiptp=Flat\n";
    }
    void producer(unsigned i) {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            const unsigned current=epoch;
            auto& strm=streams[i];
            const uint64_t cnt=stream_count(i,current);
            for(uint64_t seq=0;seq<cnt;++seq) {
                if(strm.queue.size()>=QUEUE_DEPTH) {
                    double begin=iommu_md::now_ns(); ++strm.full_waits;
                    while(strm.queue.size()>=QUEUE_DEPTH && !stopped) wait(strm.space_event);
                    strm.full_wait_ns+=iommu_md::now_ns()-begin;
                }
                if(stopped) return;
                auto r=descriptor(i,current,seq);
                r.ready_ns=iommu_md::now_ns();
                strm.queue.push_back(r);
                strm.peak_queue=std::max<uint64_t>(strm.peak_queue,strm.queue.size());
                strm.ready=true;
                ready_event.notify(SC_ZERO_TIME);
            }
            strm.done=true; previous=current;
            ready_event.notify(SC_ZERO_TIME);
        }
    }
    void scheduler() {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            unsigned current=epoch,next=RR_START;
            while(true) {
                bool all=true; for(auto& s:streams) all=all && (s.ready || s.done);
                if(all) break;
                wait(ready_event);
            }
            while(!stopped) {
                int selected=-1;
                for(unsigned k=0;k<S;++k) {
                    unsigned i=(next+k)%S;
                    if(!streams[i].queue.empty()) { selected=i; break; }
                }
                if(selected<0) {
                    bool done=true; for(auto& s:streams) done=done && s.done;
                    if(done) break;
                    wait(ready_event); continue;
                }
                unsigned i=static_cast<unsigned>(selected);
                auto& strm=streams[i];
                auto record=strm.queue.front(); strm.queue.pop_front();
                strm.space_event.notify(SC_ZERO_TIME);
                require(record.epoch==current && record.sequence==strm.scheduled,"队列内请求顺序错误");
                ++strm.scheduled;
                record.grant_ns=iommu_md::now_ns();
                auto trans=std::make_unique<tlm_generic_payload>();
                trans->set_address(record.iova);
                trans->set_data_ptr(new unsigned char[record.bytes]());
                trans->set_data_length(record.bytes);
                trans->set_streaming_width(record.bytes);
                trans->set_command(record.write ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
                trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
                auto* ext=new PayloadExtention();
                ext->requester_id=strm.dev; ext->pid_valid=0; ext->process_id=0;
                ext->exec_req=0; ext->priv_req=0; ext->no_write=record.write ? 0 : 1;
                ext->at=0; ext->is_ctrl=record.type!=0;
                trans->set_extension(ext);
                auto* ptr=trans.get();
                payloads.push_back(std::move(trans));
                top.md_trace.register_request(ptr,record);
                next=(i+1)%S;   // 顺序 i=c*N+d: 设备内层快变、通道外层慢变。
#if TEST_CFG_SPLIT_RW_PORTS
                // [读写端口分离] scheduler 不再直接 nb_transport_fw: 按方向压入读/写发送队列,
                //   由 sender_r/sender_w 发送。grant_ns 由 sender 在真正调用端口前置为发送时刻。
                if(record.write) {
                    rw_queue_w.push_back(SendItem{ptr,record,&strm});
                    rw_peak_w=std::max<uint64_t>(rw_peak_w,rw_queue_w.size());
                    rw_event_w.notify(SC_ZERO_TIME);
                } else {
                    rw_queue_r.push_back(SendItem{ptr,record,&strm});
                    rw_peak_r=std::max<uint64_t>(rw_peak_r,rw_queue_r.size());
                    rw_event_r.notify(SC_ZERO_TIME);
                }
#else
                sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
                auto status=rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr,phase,delay);
                require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
                ++strm.injected;
                std::cout << "[MDMC_GRANT] epoch=" << current << " i=" << i << " dev=" << strm.dev
                          << " chan=" << strm.chan << " seq=" << record.sequence << " type=" << record.type
                          << " grant=" << record.grant_ns << " accept=" << iommu_md::now_ns() << '\n';
#endif
            }
#if TEST_CFG_SPLIT_RW_PORTS
            // [读写端口分离] 置 sent 前等待两个 rw 队列排空且 sender 全部发完,
            //   避免 epoch 1/2 残留请求串入测量 epoch。
            while(!stopped && !(rw_queue_r.empty() && rw_queue_w.empty() && senders_idle()))
                wait(1,SC_NS);
            if(stopped) return;
            // 全部请求已送达端口 => 注入结束: 关闭 IOMMU 侧"注入并发窗口"。
            //   (窗口仅在测量 epoch 前被打开, epoch 1/2 时此调用为 no-op)
            top.conc_window_close();
            if(current==iommu_md::MEASUREMENT_EPOCH)
                std::cout << "[MDMC_INJECT_END] epoch=" << current
                          << " t=" << iommu_md::now_ns()
                          << " last_send_r=" << last_send_ns_r
                          << " last_send_w=" << last_send_ns_w << '\n';
#endif
            previous=current; sent=true;
        }
    }
#if TEST_CFG_SPLIT_RW_PORTS
    // [读写端口分离] 方向发送进程: 从对应 rw 队列取出请求, 置 grant_ns 后调用对应端口 socket
    //   的 nb_transport_fw(回调内带宽延时+并发门控自然阻塞本方向, 读写互不影响)。
    //   写方向单一 FIFO 队列天然保持每设备写注入顺序(写保序)。
    void sender(bool is_write) {
        auto& q   = is_write ? rw_queue_w : rw_queue_r;
        auto& evt = is_write ? rw_event_w : rw_event_r;
        bool& busy= is_write ? sender_busy_w : sender_busy_r;
        uint64_t& scnt = is_write ? starve_cnt_w : starve_cnt_r;
        double&   sns  = is_write ? starve_ns_w  : starve_ns_r;
        double&   last = is_write ? last_send_ns_w : last_send_ns_r;
        while(!stopped) {
            if(q.empty()) {
                // 供给侧饥饿: 本方向无请求可发(只应发生在注入开始前/全部注入结束后)
                const double t0=iommu_md::now_ns();
                ++scnt;
                wait(evt);
                sns+=iommu_md::now_ns()-t0;
                continue;
            }
            SendItem item=q.front(); q.pop_front();
            busy=true;
            const double g=iommu_md::now_ns();
            top.md_trace.request(item.ptr).grant_ns=g;   // 更新报告用权威 grant 时刻(端口发送点)
            sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
            // [读写端口分离] 读口 socket -> axi_slave_r_nb_transport_fw,
            //   写口 socket -> axi_slave_w_nb_transport_fw(两个独立注册函数)。
            auto status = is_write
                ? rp.axi_master_to_pcie_noc_0w_socket->nb_transport_fw(*item.ptr,phase,delay)
                : rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*item.ptr,phase,delay);
            require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
            ++item.strm->injected;
            busy=false;
            last=iommu_md::now_ns();
            const unsigned i=static_cast<unsigned>(item.strm-&streams[0]);
            std::cout << "[MDMC_GRANT] epoch=" << item.record.epoch << " i=" << i
                      << " dev=" << item.strm->dev << " chan=" << item.strm->chan
                      << " seq=" << item.record.sequence << " type=" << item.record.type
                      << " dir=" << (is_write?"W":"R") << " grant=" << g
                      << " accept=" << iommu_md::now_ns() << '\n';
        }
    }
#endif
    void watchdog() {
        while(!stopped) {
            wait(1000,SC_NS);
            if(top.md_trace.progress_serial!=last_progress) {
                last_progress=top.md_trace.progress_serial; last_progress_ns=iommu_md::now_ns();
            }
            if(iommu_md::now_ns()-last_progress_ns>50000 || iommu_md::now_ns()-start_ns>1000000) {
                top.md_trace.diagnostic();
                for(unsigned i=0;i<S;++i) std::cerr << "[MDMC_QUEUE] i=" << i << " dev=" << streams[i].dev
                    << " chan=" << streams[i].chan << " queued=" << streams[i].queue.size()
                    << " sent=" << streams[i].injected << " done=" << streams[i].done << '\n';
                std::cerr << "[MDMC_STATE] responded=" << total_responded
                          << " buffer=" << top.cache_sub.get_pt_dedup_buffer()->get_valid_count()
                          << " groups=" << top.prefetch_groups.size() << " ptw=" << top.ptw_active_walks.size() << '\n';
#if TEST_CFG_SPLIT_RW_PORTS
                std::cerr << "[MDMC_RWQ_STATE] rw_queue_r=" << rw_queue_r.size()
                          << " rw_queue_w=" << rw_queue_w.size()
                          << " busy_r=" << sender_busy_r << " busy_w=" << sender_busy_w << '\n';
#endif
                require(false,"多设备多通道测试超时，未释放在途payload");
            }
        }
    }
    void run_epoch(unsigned e) {
        for(auto& s:streams) {
            require(s.queue.empty(),"切换epoch时队列非空");
            s.ready=s.done=false; s.injected=s.scheduled=s.peak_queue=s.full_waits=0; s.full_wait_ns=0;
        }
#if TEST_CFG_SPLIT_RW_PORTS
        require(rw_queue_r.empty() && rw_queue_w.empty(),"切换epoch时读写发送队列非空");
        rw_peak_r=rw_peak_w=0;
        // 每 epoch 重新统计供给侧饥饿与末次发送时刻(测量 epoch 的值才用于结论)
        starve_cnt_r=starve_cnt_w=0;
        starve_ns_r=starve_ns_w=0.0;
        last_send_ns_r=last_send_ns_w=0.0;
#endif
        total_responded=0; epoch=e; sent=false; top.md_trace.progress();
        phase_event.notify(SC_ZERO_TIME);
        while(true) {
            if(sent && total_responded==total_count(e) && top.md_quiescent()) break;
            wait(1,SC_NS);
        }
        // 逐设备聚合各通道注入数, 校验发送/完成一致。
        for(unsigned d=0;d<N;++d) {
            uint64_t dev_injected=0, dev_expect=0, dev_peak=0, dev_waits=0; double dev_wait_ns=0;
            for(unsigned c=0;c<C;++c) {
                auto& s=streams[c*N+d];
                require(s.injected==stream_count(c*N+d,e),"发送/完成数量不一致");
                dev_injected+=s.injected; dev_expect+=stream_count(c*N+d,e);
                dev_peak=std::max<uint64_t>(dev_peak,s.peak_queue); dev_waits+=s.full_waits; dev_wait_ns+=s.full_wait_ns;
            }
            require(dev_injected==dev_expect,"设备级注入/期望不一致");
            std::cout << "[MDMC_RP] epoch=" << e << " did=" << d << " injected=" << dev_injected
                      << " chan_peak_queue=" << dev_peak << " full_waits=" << dev_waits
                      << " full_wait_ns=" << dev_wait_ns << '\n';
        }
        require(total_responded==total_count(e),"响应总数与期望总数不一致");
    }
public:
    explicit MultiDeviceMcScenario(RP_Module& r) : rp(r),top(*r.iommu_ptr) {}
    ~MultiDeviceMcScenario() {
        // 正常结束只在所有响应及异步工作排空后清理；异常停止不提前清理。
        for(auto& trans:payloads) {
            PayloadExtention* ext=nullptr; trans->get_extension(ext);
            trans->clear_extension(ext); delete ext;
            delete[] trans->get_data_ptr(); trans->set_data_ptr(nullptr);
        }
    }
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext=nullptr; trans.get_extension(ext);
        require(ext && ext->requester_id<N,"响应缺少有效设备ID");
        auto& r=top.md_trace.request(&trans);
        require(r.epoch==epoch,"收到旧epoch响应");
        top.md_trace.response(trans,ext->requester_id);
        ++total_responded;
    }
    void run() {
        wait(20,SC_NS);
        start_ns=last_progress_ns=iommu_md::now_ns();
        std::cout << "[MDMC_CONFIG] N=" << N << " C=" << C << " streams=" << S
                  << " IOVA_MB=" << RANGE_MB << " groups/dev=" << GROUPS
                  << " (chan base=" << GROUPS_BASE << " rem=" << GROUPS_REM << ")"
                  << " queue=" << QUEUE_DEPTH << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " rr_start=" << RR_START << " load_mode=fixed_total"
                  << " total_meas_pkts=" << total_count(iommu_md::MEASUREMENT_EPOCH) << '\n';
        setup();
        validate_nvme10();
        for(unsigned i=0;i<S;++i) sc_spawn([this,i](){ producer(i); });
        sc_spawn([this](){ scheduler(); });
#if TEST_CFG_SPLIT_RW_PORTS
        sc_spawn([this](){ sender(false); });   // sender_r: 读口发送进程(常驻, 每 epoch 不重复 spawn)
        sc_spawn([this](){ sender(true); });    // sender_w: 写口发送进程
#endif
        sc_spawn([this](){ watchdog(); });
        invalidate_all();
        run_epoch(1); // 同IOVA、不同S1/S2结果的哨兵。
        invalidate_all();
        run_epoch(2); // 预热不进入正式统计。
        // [STAT] 两项指标只统计测量 epoch: 预热结束后 IOMMU 内部 quiescent,
        //   此处重置 4KB 并发窗口与去重 Cache 有效行采样累加器。
        top.reset_4kb_concurrency_window();
        top.cache_sub.reset_dedup_valid_line_stats();
        // [STAT] 打开 IOMMU 侧稳态窗口(Steady IOPS/Theory peak/Efficiency、稳态端口带宽、
        //   稳态端口注入间隔), 与单设备8通道基线(rand4k_msi_mix_multichan)及 mps128/inval
        //   项完全同口径; 窗口按"已完成数(哨兵+预热 epoch) + 正式 epoch 的 10%~90%"精确对齐。
        //   steady_*_count 仅为统计阈值(在 reorder 中锁存时刻与字节快照), 无任何时序影响。
        //   [A/B 已验证] 该改动对 N16C8 既有死锁无因果关系: 移除本块后 N16C8 仍以 RC=134
        //   在同一时刻(75020 ns, responded=284)中止, 日志仅少 [MDMC_STAT_WINDOW] 一行。
        {
            const uint64_t done_before=top.iommu_total_completed;
            const uint64_t meas=total_count(iommu_md::MEASUREMENT_EPOCH);
            top.steady_start_count=done_before+meas*STEADY_STATE_START_PERCENT/100;
            top.steady_end_count  =done_before+meas*STEADY_STATE_END_PERCENT/100;
            std::cout << "[MDMC_STAT_WINDOW] done_before=" << done_before
                      << " meas=" << meas
                      << " steady_start_count=" << top.steady_start_count
                      << " steady_end_count=" << top.steady_end_count << '\n';
        }
        const uint64_t ptw_before=top.ptw_total_completed, ddr_before=top.axi_master_1_read_req_total;
        run_epoch(iommu_md::MEASUREMENT_EPOCH);
#if TEST_CFG_SPLIT_RW_PORTS
        std::cout << "[MDMC_RWQ] depth=" << RWQ_DEPTH << " peak_read=" << rw_peak_r
                  << " peak_write=" << rw_peak_w << '\n';
        // [供给验证] 注入期内读/写发送进程的饥饿次数与时长:
        //   starve_ns 越接近 0 => RP 侧请求越充足(端口从不因缺请求而空等)。
        std::cout << "[MDMC_SUPPLY] R starve_cnt=" << starve_cnt_r
                  << " starve_ns=" << starve_ns_r << " last_send=" << last_send_ns_r
                  << " | W starve_cnt=" << starve_cnt_w
                  << " starve_ns=" << starve_ns_w << " last_send=" << last_send_ns_w << '\n';
        // 报告前缀带 rwsplit, 与既有基线 CSV(multidev_mc_n16_c8_512mb_*)完全隔离, 不覆盖基线。
        std::string pfx = std::string("multidev_mc_")
            + (TEST_CFG_SPLIT_RW_PORTS ? "rwsplit_" : "")
            + (TEST_CFG_MD_DATA_ONLY ? "data_" : "")
            + (NVME10 ? "nvme10_" : "")
            + "n" + std::to_string(N) + "_c" + std::to_string(C) + "_" + std::to_string(RANGE_MB) + "mb";
        top.md_trace.report(pfx);
#else
        top.md_trace.report("multidev_mc_n"+std::to_string(N)+"_c"+std::to_string(C)+"_"+std::to_string(RANGE_MB)+"mb");
#endif
        std::cout << "[MDMC_DELTA] PTW=" << top.ptw_total_completed-ptw_before
                  << " DDR_reads=" << top.axi_master_1_read_req_total-ddr_before << '\n';
        // 与单设备基线同口径的IOMMU侧统计，便于横向对比。
        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)top.ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)top.ptw_total_ddr_reads);
        if (top.ptw_total_completed > 0)
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)top.ptw_total_ddr_reads / top.ptw_total_completed);
        printf("================================================\n");
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
            printf("  总持有延时 = W1+W2+W3; W1=alloc->group_complete, W2=group_complete->agg_flush_exec, W3=agg_flush_exec->entry_free\n");
            auto pr_hold_mc = [](const char* label, uint64_t n, const double* sum, const double* mx) {
                printf("  %-30s n=%-7lu avg total=%8.1f ns (W1=%7.1f W2=%6.1f W3=%5.1f) | maxSeg %.1f/%.1f/%.1f\n",
                       label, (unsigned long)n,
                       n ? (sum[0]+sum[1]+sum[2])/(double)n : 0.0,
                       n ? sum[0]/(double)n : 0.0, n ? sum[1]/(double)n : 0.0, n ? sum[2]/(double)n : 0.0,
                       mx[0], mx[1], mx[2]);
            };
            pr_hold_mc("MAIN head (dedup MISS)", top.md_hold_main_cnt, top.md_hold_main_sum, top.md_hold_main_max);
            pr_hold_mc("MAIN head (PF placeholder)",  top.md_hold_pf_cnt,  top.md_hold_pf_sum,  top.md_hold_pf_max);
            pr_hold_mc("SUSPENDED (deduped)",         top.md_hold_susp_cnt, top.md_hold_susp_sum, top.md_hold_susp_max);
            printf("======================================================\n");
        }
        top.print_cache_statistics();
        stopped=true; phase_event.notify(SC_ZERO_TIME); ready_event.notify(SC_ZERO_TIME);
        for(auto& s:streams) s.space_event.notify(SC_ZERO_TIME);
        std::cout << "[MULTIDEV_MC_RESULT] PASS expected=" << total_count(iommu_md::MEASUREMENT_EPOCH)
                  << " completed=" << total_count(iommu_md::MEASUREMENT_EPOCH) << "\n";
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario=std::make_shared<MultiDeviceMcScenario>(*this);
    md_response_handler=[scenario](tlm_generic_payload& trans){ scenario->response(trans); };
    scenario->run();
}
#endif
