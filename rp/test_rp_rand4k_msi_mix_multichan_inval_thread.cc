#include "test_rp.hh"
#include "iommu_top.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include <array>
#include <cmath>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <numeric>

// ============================================================================
// 单设备-单通道-strict/lazy 模式缓存失效场景 (CQ 反压闭环)
//
// 激励蓝本: rand4k_msi_mix_multichan(N=1, fixed_total) => 单设备/单通道/512MB/
//   读写端口分离, 909 组 NVMe 任务 + 1 tail MSI = 10000 包。
//   每组 NVMe 任务 = 1 读 SQE + 8 个 512B TLP 读写包 + 1 写 CQE + 1 写 MSI。
//
// 三个并发进程与一条 IOMMU 侧命令线程构成闭环:
//   (a) 请求发送进程 producer/scheduler/sender: 注入 NVMe 组;
//   (b) IOMMU 出口记录点 track_4kb_page_out(TEST_CFG_CPU_INVAL_SCENE hook):
//       同一4KB页数据任务全部完成的时刻(不是CQE/MSI返回时刻),
//       把 {page_iova, done_ns} 压入 top.cpu_inval_done_pages 并 notify;
//   (c) CPU 缓存失效命令发送进程: 消费完成事件(软件处理 MSI 中断), 延时
//       INVAL_DELAY_NS(默认 32ns)后经 CQ 下发失效命令:
//         strict(TEST_CFG_INVAL_MODE=0): 每笔完成事件 -> 1 笔
//           IOTINVAL.VMA **010** (GV=0,AV=1,PSCV=0,NL=0,ADDR=页IOVA)
//           -> 扫描缓存表匹配 addr, 只失效 PT Cache 中匹配的 cacheline,
//              NL=0 故**不失效 Walker Cache**;
//         lazy  (TEST_CFG_INVAL_MODE=1): 每 INVAL_LAZY_N 笔完成事件 -> 1 笔
//           IOTINVAL.VMA **100** (GV=1,AV=0,PSCV=0,GSCID=1)
//           -> 扫描缓存表匹配 GSCID, 清除 PT Cache 与 Walker Cache 中所有
//              GSCID 匹配的行(本场景全部行 GSCID=1, 等价于整表清除),
//              并由 IOMMU 内部**启动延时失效策略**(LIB 记录 + VN 递增,
//              查询旁路比对生效, VN 回绕触发批量扫表);
//   (d) IOMMU cq_async_process_thread: 门铃驱动逐笔取指 -> 阻塞执行失效流水线
//       -> 推进 CQH。
//
// CQ 深度为 INVAL_CQ_DEPTH(默认64, 留一空槽, 可用63)。
//   CPU始终只在真实CQ满时等待CQH；命令投递与执行异步解耦。
//   strict保留历史64组信用窗口，每次CQH前进补1组，本次不改变其行为。
//   lazy取消初始64组窗口：CQ未满时持续供给；仅在实际发送端发现CQ满时
//   暂停新组准入。满期间每次CQH前进允许N组(读写口共享额度)，队列恢复
//   未满即恢复自由供给。已准入组的剩余包继续排空，不重发或撤销在途包。
//   正常端口带宽、在途数和FIFO反压保持不变；信用耗尽不再冒充真实CQ满。
//
// 本文件仅在 TEST=rand4k_msi_mix_multichan_inval_strict/lazy 时经 TEST_THREAD_SRC
// 编入; 其余场景不编译, 对基线零影响。
// 复用多设备追踪(md_trace/md_response_handler), 需 Makefile 同时置
// TEST_CFG_MULTI_DEVICE_SCENE=1 + TEST_CFG_NUM_DEVICES=1。
// ============================================================================

// ---- 失效场景编译宏安全默认(Makefile 正常注入; 此处仅兜底, 不改共享头) ----
#ifndef TEST_CFG_MULTI_CHANNEL_INVAL_SCENE
#define TEST_CFG_MULTI_CHANNEL_INVAL_SCENE 0
#endif
#ifndef TEST_CFG_INVAL_MODE                // 0=strict(每4KB页一笔VMA 010精准失效); 1=lazy(每N页一笔VMA 100全局失效)
#define TEST_CFG_INVAL_MODE 0
#endif
#ifndef TEST_CFG_INVAL_DELAY_NS            // 4KB页完成(软件收到MSI) -> 发起失效命令 的延时(ns)
#define TEST_CFG_INVAL_DELAY_NS 32
#endif
#ifndef TEST_CFG_INVAL_LAZY_N              // lazy 模式: 每 N 个 4KB 页完成发一笔全局失效
#define TEST_CFG_INVAL_LAZY_N 32
#endif
#ifndef TEST_CFG_INVAL_CQ_DEPTH            // IOMMU 命令队列(CQ)深度(条目数, 必须为2的幂)
#define TEST_CFG_INVAL_CQ_DEPTH 8192
#endif
#ifndef TEST_CFG_CPU_COUNT                 // [多核] CPU 核数 = 请求发送进程数 = CPU失效命令进程数
#define TEST_CFG_CPU_COUNT 1
#endif
#ifndef TEST_CFG_MULTI_CHANNEL_SCENE
#define TEST_CFG_MULTI_CHANNEL_SCENE 0
#endif
#ifndef TEST_CFG_NUM_CHANNELS
#define TEST_CFG_NUM_CHANNELS 1
#endif
#ifndef TEST_CFG_MC_QUEUE_DEPTH
#define TEST_CFG_MC_QUEUE_DEPTH 32
#endif
#ifndef TEST_CFG_MC_RR_START
#define TEST_CFG_MC_RR_START 0
#endif
#ifndef TEST_CFG_MC_LOAD_MODE            // 0=per_channel(每通道GROUPS组); 1=fixed_total(总组数=NUM_PAGES)
#define TEST_CFG_MC_LOAD_MODE 0
#endif
#ifndef TEST_CFG_NUM_PAGES               // 兜底: 每通道组数(per_channel)或总组数(fixed_total)
#define TEST_CFG_NUM_PAGES 909
#endif
#ifndef TEST_CFG_MC_RW_QUEUE_DEPTH       // [读写端口分离] 读/写发送队列深度(近乎无界)
#define TEST_CFG_MC_RW_QUEUE_DEPTH 65536
#endif

// 失效场景复用多设备追踪(md_trace/md_response_handler), 必须同时开启 MULTI_DEVICE_SCENE;
// 且依赖 IOMMU 侧 4KB 页完成记录点(TEST_CFG_CPU_INVAL_SCENE)。
#if TEST_CFG_MULTI_CHANNEL_INVAL_SCENE && !TEST_CFG_MULTI_DEVICE_SCENE
#error "TEST_CFG_MULTI_CHANNEL_INVAL_SCENE 需要 TEST_CFG_MULTI_DEVICE_SCENE=1 (复用 md_trace)"
#endif
#if TEST_CFG_MULTI_CHANNEL_INVAL_SCENE && !TEST_CFG_CPU_INVAL_SCENE
#error "TEST_CFG_MULTI_CHANNEL_INVAL_SCENE 需要 TEST_CFG_CPU_INVAL_SCENE=1 (IOMMU 侧 4KB 完成记录点)"
#endif

#if TEST_CFG_MULTI_CHANNEL_INVAL_SCENE
namespace {
using iommu_md::require;
// [多核] NCHAN = CPU 核数: 每个“通道”= 一个 CPU 核(1 个请求发送进程 + 1 个 CPU失效命令进程)。
//   输出仍为单通道(1 scheduler + sender_r + sender_w, 出口轮询仲裁)。
constexpr unsigned NCHAN = TEST_CFG_CPU_COUNT;
constexpr bool INVAL_LAZY = (TEST_CFG_INVAL_MODE != 0);
constexpr double INVAL_DELAY_NS = (double)TEST_CFG_INVAL_DELAY_NS;
constexpr unsigned INVAL_LAZY_N = TEST_CFG_INVAL_LAZY_N;
static_assert(!INVAL_LAZY || INVAL_LAZY_N > 0, "lazy 模式 N 必须为正");
// [CQ反压] IOMMU 命令队列深度(条目数)。规范满条件 cqt==(cqh-1) mod size,
//   故实际可用深度 = INVAL_CQ_DEPTH - 1; 初始注入信用取 INVAL_CQ_DEPTH,
//   多出的 1 个额度会被 CPU 进程的满检查自然吸收(此时 1 笔完成事件在 RP 侧待写)。
constexpr unsigned INVAL_CQ_DEPTH = TEST_CFG_INVAL_CQ_DEPTH;
static_assert(INVAL_CQ_DEPTH >= 2 && (INVAL_CQ_DEPTH & (INVAL_CQ_DEPTH - 1)) == 0,
              "CQ 深度必须为 >=2 的 2 的幂");
constexpr unsigned QUEUE_DEPTH = TEST_CFG_MC_QUEUE_DEPTH;
constexpr uint64_t RWQ_DEPTH = TEST_CFG_MC_RW_QUEUE_DEPTH;   // [读写端口分离] 发送队列深度上限(近乎无界, 仅记录峰值)
// [多核] 强制 per_channel 口径: 每核独立发 NUM_PAGES(909) 组 = 10000 包, 总包数 = NCHAN x 10000。
//   (复用单设备-单通道512Byte场景测试项: 每个请求发送进程都跑完整的 10000 包。)
constexpr bool FIXED_TOTAL = false;
constexpr unsigned RR_START = TEST_CFG_MC_RR_START;
// 负载口径:
//   per_channel(FIXED_TOTAL=0): 每通道 NUM_PAGES 组 + 1 tail MSI = 10000 包/通道, 总 n x 10000 包;
//   fixed_total (FIXED_TOTAL=1): 全部通道合计 NUM_PAGES(909) 组, 余数分给前 GROUPS_REM 个通道
//               (n=8: 114x5 + 113x3), 末尾 1 个 tail MSI 只给通道0 -> 总包数 = 909x11 + 1 = 10000,
//               与单流基线逐包同量。默认口径: 总包数=基线, 用于对比性能变化;
//               且工作集小(建表~3.5k页), 可规避 per_channel 大工作集触发的两阶段预取无效页问题。
constexpr unsigned GROUPS_BASE = FIXED_TOTAL ? (TEST_CFG_NUM_PAGES / NCHAN) : TEST_CFG_NUM_PAGES;
constexpr unsigned GROUPS_REM  = FIXED_TOTAL ? (TEST_CFG_NUM_PAGES % NCHAN) : 0u;
constexpr unsigned MAX_CHAN_GROUPS = GROUPS_BASE + (GROUPS_REM > 0u ? 1u : 0u);
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
constexpr uint64_t PROBE_IOVA = SQ_IOVA + 0x10000;
constexpr uint64_t HUGE = 0x200000ULL;
static_assert(NCHAN >= 1 && NCHAN <= 32, "CPU 核数必须为1..32(MAX_CPU_COUNT)");
static_assert(QUEUE_DEPTH > 0 && QUEUE_DEPTH <= 1048576, "就绪队列深度非法");
static_assert(MAX_CHAN_GROUPS > 0 && MAX_CHAN_GROUPS <= RANGE_PAGES, "组数超过候选页数");
static_assert(PT_DEDUP_PREFETCH_DEPTH <= 16, "预取深度超过结果槽容量");
static_assert(RR_START < NCHAN, "轮询起点非法");
static_assert(!FIXED_TOTAL || NCHAN == 1 || NCHAN == 2 || NCHAN == 4 || NCHAN == 8 || NCHAN == 16,
              "等总负载模式要求n为1/2/4/8/16");

// 单一共享设备: SPA 偏移与 MSI PA 均取 device_id=0。
uint64_t spa_offset(unsigned d) { return 0x80000000ULL + uint64_t(d) * 0x10000000ULL; }
uint64_t msi_pa(unsigned d, unsigned v) { return 0x40000000ULL + uint64_t(d)*0x100000ULL + uint64_t(v)*4096 + 0x40; }

// 每通道组数/tail数: fixed_total 下前 GROUPS_REM 个通道各多1组, 且仅通道0补1个tail MSI,
// 使 total_count(3) 精确等于 NUM_PAGES*11+1 (=10000, 与单流基线一致)。
constexpr unsigned chan_groups(unsigned c) { return GROUPS_BASE + (c < GROUPS_REM ? 1u : 0u); }
constexpr unsigned chan_tails(unsigned c) { return FIXED_TOTAL ? (c == 0u ? 1u : 0u) : 1u; }
constexpr uint64_t chan_count(unsigned c, unsigned e) {
    return e == 1 ? 2 : e == 2 ? 16 : uint64_t(chan_groups(c)) * 11 + chan_tails(c);
}
constexpr uint64_t total_count(unsigned e) {
    uint64_t s = 0;
    for (unsigned c = 0; c < NCHAN; ++c) s += chan_count(c, e);
    return s;
}

// [多核信用反压] 每个 CPU 核独立的注入信用状态(替代原 CQ-full-only 门控)。
//   信用定义: 1 信用 = 1 组 NVMe 任务(11 包)的注入权。
//   闭环: 请求发送进程 i 初始持有 (strict:1 / lazy:N) 组信用; 每下发一组消耗 1 信用;
//     信用耗尽即被反压(等待), 直到 CPU 失效进程 i 监控到本核 nvme_id 的任务完成、
//     下发 invalid+iofence 且 IOMMU 处理完成(CQ head 越过这两条命令)后释放
//     (strict:+1 组 / lazy:+N 组)信用。核间完全独立: CPU_i 只反压 sender_i。
struct CpuCredit {
    int      credit = 0;             // 本核还可注入的组数
    sc_event release_event;          // 本核补信用时 notify(唤醒对应 sender)
    uint64_t groups_sent = 0;        // 本核已放行(消耗信用)的组数
    uint64_t groups_released = 0;    // 本核经 CPU 失效进程释放的组数(累计)
    uint64_t block_count = 0;        // 本核 sender 因信用耗尽被反压的次数
    double   blocked_ns = 0.0;       // 本核 sender 反压累计时长
    double   blocked_max_ns = 0.0;   // 本核 sender 单次最长反压
};

// 每通道(=每CPU核)只持有独立的随机页选择与就绪队列; 设备上下文/VS映射/MSI/控制包全场景共享。
struct Channel {
    std::vector<unsigned> pages;
    std::deque<iommu_md::Request> queue;
    sc_event space_event;
    bool ready = false, done = false;
    uint64_t injected = 0, scheduled = 0, peak_queue = 0, full_waits = 0;
    double full_wait_ns = 0;
};

// 所有SystemC进程属于单个RP，只有scheduler访问唯一的initiator socket。
class MultiChannelScenario {
    RP_Module& rp;
    iommu_top& top;
    // ---- 单一共享设备状态(device_id=0, GSCID=1) ----
    device_context_t dc{};
    uint64_t dc_address = 0, msi_iova = 0, probe_gpa = 0;
    std::vector<unsigned> msi_pages;
    std::vector<uint64_t> gpas;               // 共享VS映射: gpas[RANGE_PAGES]
    // ---- n 路并发通道 ----
    std::array<Channel, NCHAN> channels;
    uint64_t total_responded = 0;
    std::vector<std::unique_ptr<tlm_generic_payload>> payloads;
    sc_event phase_event, ready_event;
    unsigned epoch = 0;
    bool sent = false, stopped = false;
    double start_ns = 0;
    uint64_t last_progress = 0;
    double last_progress_ns = 0;

    // [多核信用反压] 每核独立注入信用 + CQ 写入串行化锁。
    //   多核共享单一 CQ(深度 8192), 每次写入 invalid+iofence 两条命令须原子,
    //   故写入时按仲裁逻辑选中某核并 lock(cq_write_mtx), 写完 invalid+iofence 后 unlock。
    std::array<CpuCredit, NCHAN> cpu_credit;
    sc_mutex cq_write_mtx;

#if TEST_CFG_SPLIT_RW_PORTS
    // [读写端口分离] scheduler 按 RR 选出的请求按方向压入读/写发送队列, 由 sender_r/sender_w
    //   各自调用对应端口 socket 的 nb_transport_fw 发送。回调内的带宽延时+并发门控自然阻塞
    //   本方向发送进程, 读写互不影响; 写方向 FIFO 天然保持每设备写注入顺序(写保序)。
    struct SendItem {
        tlm_generic_payload* ptr;      // 已 register_request 的 payload
        iommu_md::Request    record;   // 请求副本(用于发送后打印/计数)
        Channel*             chan;     // 所属通道(++injected)
    };
    std::deque<SendItem> rw_queue_r, rw_queue_w;
    sc_event rw_event_r, rw_event_w;
    bool sender_busy_r = false, sender_busy_w = false;   // sender 正在 nb_transport_fw 内阻塞
    uint64_t rw_peak_r = 0, rw_peak_w = 0;               // 两队列峰值占用(报告用)
    // [供给验证] 发送侧饥饿统计: sender 因本方向队列为空而等待的次数/时长。
    //   注入窗口内 starve_ns≈0 即证明 RP 供给侧始终有请求可发,
    //   IOMMU 端口不会因缺请求而空闲(读写均可持续尝试准入)。
    uint64_t starve_cnt_r = 0, starve_cnt_w = 0;
    double   starve_ns_r = 0.0, starve_ns_w = 0.0;
    double   last_send_ns_r = 0.0, last_send_ns_w = 0.0;  // 各口最后一次送出请求的时刻
    bool senders_idle() const { return !sender_busy_r && !sender_busy_w; }
    // sender(bool is_write) 定义于下方 scheduler 之后(类内联定义, 不在此重复声明)
#endif

    static std::vector<unsigned> choose(unsigned count, unsigned range, unsigned seed, unsigned shuffle_seed) {
        std::mt19937 rng(seed), permutation(shuffle_seed);
        std::set<unsigned> chosen;
        while (chosen.size() < count) chosen.insert(rng() % range);
        std::vector<unsigned> result(chosen.begin(), chosen.end());
        for (size_t i=result.size();i>1;--i) std::swap(result[i-1],result[permutation()%i]);
        return result;
    }
    void map_vs(uint64_t iova, uint64_t gpa) {
        spte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=gpa/4096;
        require(rp.add_vs_stage_pte(&top,dc.fsc.iosatp,iova,p,0,dc.iohgatp,0)!=uint64_t(-1),
                "构建VS页表失败");
    }
    void map_g(uint64_t gpa, uint64_t spa, unsigned level) {
        gpte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=spa/4096;
        require(rp.add_g_stage_pte(&top,dc.iohgatp,gpa,p,level)!=uint64_t(-1), "构建G-stage失败");
    }
    void setup() {
        // 保留现有低地址寄存器/CQ区域，只初始化一次物理分配游标。
        next_free_page=std::max<uint64_t>(next_free_page,240);
        // 单设备共享页表预算(远小于多设备 N x): 每2MiB IOVA区一张L0表 + 目录/G-stage/MSI裕量。
        //   [多核] CQ 深度 8192 条目(16B/条)需 32 物理页(128KB), 预算额外预留 cq_pages+32(对齐余量)。
        const uint64_t device_pages=(uint64_t(RANGE_MB)+2)/2+64;
        const uint64_t cq_pages=(uint64_t(INVAL_CQ_DEPTH)*16 + 4095)/4096;   // CQ 条目 16B/条
        require((next_free_page+cq_pages+32+device_pages)*4096 <= DDR_Module::DDR_MEMORY_SIZE,
                "单设备页表预算超过32MiB DDR");
        require(rp.enable_iommu(&top,DDT_1LVL)>=0,"启用IOMMU失败");
        // [CPU_INVAL] 使能命令队列并显式指定深度(条目数): 失效命令必须经 CQ 才被
        //   IOMMU 执行(未使能时 cqon=0/cqen=0, 命令处理线程直接返回)。
        //   [多核] 深度 = INVAL_CQ_DEPTH(默认 8192) => cqb.log2szm1 = log2(8192)-1 = 12,
        //   IOMMU 侧 qsize = 1<<(log2szm1+1) = 8192, 规范满条件占用 8191 条。
        //   物理页按 cq_pages(8192 条目 => 32 页)分配, 由 enable_cq 的 nppn 参数指定。
        //   仅 1 个 CQ, 多核共享(写入按 cq_write_mtx 仲裁 + lock); head 由 IOMMU 硬件逻辑更新(仅 1 套)。
        require(rp.enable_cq(&top,cq_pages,INVAL_CQ_DEPTH)>=0,"启用命令队列失败");
        top.cq_depth_entries = INVAL_CQ_DEPTH;
        require(top.cq_qsize()==INVAL_CQ_DEPTH,"CQ 深度配置未生效(qsize 与预期不符)");
        // [多核信用反压] 原"CQ 满才反压 iommu 入口"机制已去掉: 改为严格 CPU 控制下发任务,
        //   每核 sender_i 由 CPU 失效进程 i 通过信用(初始 strict:1 / lazy:N 组)反压。
        //   CQ 深度 8192 远大于稳态在途命令数(<= 2 x cpu_count), 理论上永不满;
        //   若真满(占用达 usable)则视为错误(见 cpu_inval_process 的 require)。
        top.cq_credit_gate_enabled = false;   // 信用池门控保持关闭(反压改由每核 CpuCredit 承担)
        top.cq_credit_per_cmd = INVAL_LAZY ? INVAL_LAZY_N : 1u;  // 仅供 IOMMU 侧报告展示(门控已关)
        std::cout << "[CPU_INVAL] Command queue enabled (depth=" << INVAL_CQ_DEPTH
                  << " entries, usable=" << top.cq_usable_depth()
                  << ", log2szm1=" << unsigned(top.iommu_inst.reg_file.cqb.log2szm1)
                  << ", phys_pages=" << cq_pages
                  << ") gate=PER_CPU_CREDIT cpu_count=" << NCHAN << "\n";

        // ---- 单一共享设备上下文(device_id=0, GSCID=1, Sv48x4/Sv48, MSIPTP Flat) ----
        next_free_gpage[1]=0x10000;
        probe_gpa=0x100000ULL;
        msi_pages=choose(256,1024,777,777);
        msi_iova=MSI_IOVA_BASE+uint64_t(msi_pages[0])*4096+0x40;
        dc_address=rp.add_device(&top,0,1,0,0,0,0,0,1,1,0,0,0,
                                 IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Flat,1,0xFF,MSI_PATTERN);
        require(dc_address!=uint64_t(-1),"添加设备失败");
        rp.read_memory_test_rp(dc_address,sizeof(dc),reinterpret_cast<char*>(&dc));
        dc.ta.PSCID=0;
        rp.write_memory_test_rp(reinterpret_cast<char*>(&dc),dc_address,sizeof(dc));
        device_context_t readback{};
        rp.read_memory_test_rp(dc_address,sizeof(readback),reinterpret_cast<char*>(&readback));
        require(readback.iohgatp.GSCID==1 && readback.ta.PSCID==0 && readback.tc.PDTV==0,
                "DC读回上下文不匹配");

        // G-stage: HUGE_COUNT 个 2MB 大页(GPA 0~RANGE), SPA=GPA+spa_offset(0)。
        for(unsigned h=0;h<HUGE_COUNT;++h) map_g(uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(0),1);
        // MSIPT(Flat, 256 vectors) + MSI IOVA->GPA 映射, 全通道共享。
        for(unsigned v=0;v<256;++v) {
            msipte_t mp{};
            mp.V=1; mp.M=3; mp.translate_rw.PPN=msi_pa(0,v)/4096;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&mp),dc.msiptp.PPN*4096+v*16,16);
            map_vs(MSI_IOVA_BASE+uint64_t(msi_pages[v])*4096,(MSI_PATTERN<<12)+uint64_t(v)*4096);
        }
        // 共享 VS 映射 gpas[RANGE_PAGES]: 512MB 多对一随机 GPA(同基线)。
        gpas.resize(RANGE_PAGES);
        std::mt19937 mapping(42);
        std::array<unsigned,HUGE_COUNT> slots{};
        for(unsigned i=0;i<RANGE_PAGES;++i) {
            unsigned h=mapping()%HUGE_COUNT, slot=0;
            if(RANGE_MB==16) {
                while(slots[h]>=512) h=(h+1)%HUGE_COUNT;
                slot=slots[h]++;
            } else slot=mapping()%512;
            gpas[i]=uint64_t(h)*HUGE+uint64_t(slot)*4096;
        }
        // 每通道独立随机选 chan_groups(c) 个 4KB 页(确定性可复现, 通道间可重叠)。
        for(unsigned c=0;c<NCHAN;++c)
            channels[c].pages=choose(chan_groups(c),RANGE_PAGES,2024+c,123+c);
        // VS 建表 = 所有通道 pages 的并集 + 各自 D 个预取相邻页。
        std::set<unsigned> mapped;
        if(RANGE_MB==16) {
            for(unsigned i=0;i<RANGE_PAGES;++i) mapped.insert(i);
        } else {
            for(unsigned c=0;c<NCHAN;++c)
                for(unsigned page:channels[c].pages) {
                    unsigned vpn0=((IOVA_BASE>>12)+page)&511;
                    unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-vpn0);
                    for(unsigned k=0;k<=depth && page+k<RANGE_PAGES;++k) mapped.insert(page+k);
                }
        }
        for(unsigned page:mapped) map_vs(IOVA_BASE+uint64_t(page)*4096,gpas[page]);
        // 控制包共享且不变: SQ(32B读)/CQ(16B写) 只映射一次, 全通道复用。
        map_vs(SQ_IOVA,CTRL_GPA);
        map_vs(CQ_IOVA,CTRL_GPA+4096);
        map_g(CTRL_GPA,CTRL_GPA+spa_offset(0),0);
        map_g(CTRL_GPA+4096,CTRL_GPA+4096+spa_offset(0),0);
        // 独立哨兵 IOVA(不计入正式性能请求) + 预取相邻页占位。
        map_vs(PROBE_IOVA,probe_gpa);
        for(uint64_t base:{SQ_IOVA,CQ_IOVA,PROBE_IOVA}) {
            unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-((base>>12)&511));
            for(unsigned k=1;k<=depth;++k) {
                uint64_t page=base+uint64_t(k)*4096;
                if(page==SQ_IOVA || page==CQ_IOVA || page==PROBE_IOVA) continue;
                map_vs(page,0x200000ULL+uint64_t(k)*4096);
            }
        }
        uint64_t digest=1469598103934665603ULL;
        for(unsigned c=0;c<NCHAN;++c)
            for(unsigned page:channels[c].pages) digest=(digest^page)*1099511628211ULL;
        std::cout << "[MC_SETUP] did=0 GSCID=1 PSCID=0 channels=" << NCHAN
                  << " groups_base=" << GROUPS_BASE << " groups_rem=" << GROUPS_REM
                  << " mapped_pages=" << mapped.size()
                  << " sequence_hash=" << digest << " root=" << dc.iohgatp.PPN
                  << " free_ppn=" << next_free_page << '\n';
        top.md_trace.progress();
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
    iommu_md::Request descriptor(unsigned c,unsigned e,uint64_t seq) {
        auto& chan=channels[c];
        iommu_md::Request r;
        r.device=0; r.epoch=e; r.sequence=seq;   // 全部通道同属 device_id=0。
        r.nvme_id=c;   // [多核] NVMe 组号 = CPU 核 id, 组内 11 包每包携带, 贯穿翻译流水。
        if(e==1) {
            r.bytes=512; r.iova=PROBE_IOVA; r.expected_pa=probe_gpa+spa_offset(0);
            return r;
        }
        unsigned type;
        if(e==2) type=seq<8 ? 3 : (seq%2 ? 2 : 1);   // 预热: MSI(x8) + SQ/CQ 交替。
        else {
            r.group=seq/11;
            unsigned offset=seq%11;
            type=seq>=uint64_t(chan_groups(c))*11 ? 3 : offset<8 ? 0 : offset-7;
            if(type==0) {
                unsigned page=chan.pages[r.group];
                r.iova=IOVA_BASE+uint64_t(page)*4096+offset*512;
                r.expected_pa=gpas[page]+spa_offset(0)+offset*512;
                r.bytes=512; r.write=(r.group%2)!=0;   // 奇读偶写交替(组号%2)。
            }
        }
        r.type=type;
        if(type==1) { r.iova=SQ_IOVA; r.expected_pa=CTRL_GPA+spa_offset(0); r.bytes=32; }
        if(type==2) { r.iova=CQ_IOVA; r.expected_pa=CTRL_GPA+4096+spa_offset(0); r.bytes=16; r.write=true; }
        if(type==3) { r.iova=msi_iova; r.expected_pa=msi_pa(0,0); r.bytes=4; r.write=true; }
        return r;
    }
    // ====================================================================
    // [多核信用反压] 组注入信用门控已移至每核 CpuCredit(cpu_credit[] 类成员)。
    //   producer(c) 在测量 epoch 的每个 NVMe 组首包处检查 cpu_credit[c].credit,
    //   耗尽则等待 cpu_credit[c].release_event(由 cpu_inval_process(c) 释放)。
    //   原 CQ-full-only 门控(LazyCqGate/inval_before_send/acquire_inject_credit)已删除。
    // ====================================================================

    void producer(unsigned c) {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            const unsigned current=epoch;
            auto& chan=channels[c];
            for(uint64_t seq=0;seq<chan_count(c,current);++seq) {
                // [多核信用反压] 测量 epoch 的每个 NVMe 组首包: 检查本核信用, 耗尽则等待
                //   CPU 失效进程 c 释放(invalid+iofence 完成)。仅门控真实数据组首包
                //   (seq%11==0 且 seq < 组数x11); tail MSI 与 epoch1/2 不门控(无信用来源会死锁)。
                if(current==iommu_md::MEASUREMENT_EPOCH && (seq%11)==0 &&
                   seq<uint64_t(chan_groups(c))*11) {
                    const double gt0=iommu_md::now_ns();
                    bool waited=false;
                    while(!stopped && cpu_credit[c].credit<=0) {
                        if(!waited){ waited=true; ++cpu_credit[c].block_count; }
                        wait(cpu_credit[c].release_event);
                    }
                    if(stopped) return;
                    if(waited) {
                        const double dt=iommu_md::now_ns()-gt0;
                        cpu_credit[c].blocked_ns+=dt;
                        if(dt>cpu_credit[c].blocked_max_ns) cpu_credit[c].blocked_max_ns=dt;
                    }
                    cpu_credit[c].credit--;
                    cpu_credit[c].groups_sent++;
                }
                if(stopped) return;
                if(chan.queue.size()>=QUEUE_DEPTH) {
                    double begin=iommu_md::now_ns(); ++chan.full_waits;
                    while(chan.queue.size()>=QUEUE_DEPTH && !stopped) wait(chan.space_event);
                    chan.full_wait_ns+=iommu_md::now_ns()-begin;
                }
                if(stopped) return;
                auto r=descriptor(c,current,seq);
                r.ready_ns=iommu_md::now_ns();
                chan.queue.push_back(r);
                chan.peak_queue=std::max<uint64_t>(chan.peak_queue,chan.queue.size());
                chan.ready=true;
                ready_event.notify(SC_ZERO_TIME);
            }
            chan.done=true; previous=current;
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
                bool all=true; for(auto& c:channels) all=all && (c.ready || c.done);
                if(all) break;
                wait(ready_event);
            }
            while(!stopped) {
                int selected=-1;
                for(unsigned k=0;k<NCHAN;++k) {
                    unsigned c=(next+k)%NCHAN;
                    if(!channels[c].queue.empty()) { selected=c; break; }
                }
                if(selected<0) {
                    bool done=true; for(auto& c:channels) done=done && c.done;
                    if(done) break;
                    wait(ready_event); continue;
                }
                unsigned c=static_cast<unsigned>(selected);
                auto& chan=channels[c];
                auto record=chan.queue.front(); chan.queue.pop_front();
                chan.space_event.notify(SC_ZERO_TIME);
                require(record.epoch==current && record.sequence==chan.scheduled,"队列内请求顺序错误");
                ++chan.scheduled;
                record.grant_ns=iommu_md::now_ns();
                auto trans=std::make_unique<tlm_generic_payload>();
                trans->set_address(record.iova);
                trans->set_data_ptr(new unsigned char[record.bytes]());
                trans->set_data_length(record.bytes);
                trans->set_streaming_width(record.bytes);
                trans->set_command(record.write ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
                trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
                auto* ext=new PayloadExtention();
                ext->requester_id=0; ext->pid_valid=0; ext->process_id=0;   // 单一设备 device_id=0。
                ext->exec_req=0; ext->priv_req=0; ext->no_write=record.write ? 0 : 1;
                ext->at=0; ext->is_ctrl=record.type!=0;
                ext->nvme_id=record.nvme_id;   // [多核] 携带 NVMe 组号(=CPU核id)贯穿翻译流水
                trans->set_extension(ext);
                auto* ptr=trans.get();
                payloads.push_back(std::move(trans));
                top.md_trace.register_request(ptr,record);
                next=(c+1)%NCHAN;
#if TEST_CFG_SPLIT_RW_PORTS
                // [读写端口分离] scheduler 不再直接 nb_transport_fw: 按方向压入读/写发送队列,
                //   由 sender_r/sender_w 发送。grant_ns 由 sender 在真正调用端口前置为发送时刻。
                if(record.write) {
                    rw_queue_w.push_back(SendItem{ptr,record,&chan});
                    rw_peak_w=std::max<uint64_t>(rw_peak_w,rw_queue_w.size());
                    rw_event_w.notify(SC_ZERO_TIME);
                } else {
                    rw_queue_r.push_back(SendItem{ptr,record,&chan});
                    rw_peak_r=std::max<uint64_t>(rw_peak_r,rw_queue_r.size());
                    rw_event_r.notify(SC_ZERO_TIME);
                }
#else
                // [多核信用反压] 组注入信用已在 producer(c) 门控, 此处不再重复门控。
                if(stopped) return;
                top.md_trace.request(ptr).grant_ns=iommu_md::now_ns();
                sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
                auto status=rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr,phase,delay);
                require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
                ++chan.injected;
                std::cout << "[MD_GRANT] epoch=" << current << " chan=" << c << " did=0 seq=" << record.sequence
                          << " type=" << record.type << " grant=" << record.grant_ns
                          << " accept=" << iommu_md::now_ns() << '\n';
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
                std::cout << "[MC_INJECT_END] epoch=" << current
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
    //   写方向单一 FIFO 队列天然保持写注入顺序(写保序)。
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
            // [多核信用反压] 组注入信用已在 producer(c) 门控, sender 不再重复门控。
            if(stopped) return;
            const double g=iommu_md::now_ns();
            top.md_trace.request(item.ptr).grant_ns=g;   // 更新报告用权威 grant 时刻(端口发送点)
            sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
            // [读写端口分离] 读口 socket -> axi_slave_r_nb_transport_fw,
            //   写口 socket -> axi_slave_w_nb_transport_fw(两个独立注册函数)。
            auto status = is_write
                ? rp.axi_master_to_pcie_noc_0w_socket->nb_transport_fw(*item.ptr,phase,delay)
                : rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*item.ptr,phase,delay);
            require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
            ++item.chan->injected;
            busy=false;
            last=iommu_md::now_ns();
            const unsigned c=static_cast<unsigned>(item.chan-&channels[0]);
            std::cout << "[MD_GRANT] epoch=" << item.record.epoch << " chan=" << c << " did=0 seq=" << item.record.sequence
                      << " type=" << item.record.type << " dir=" << (is_write?"W":"R") << " grant=" << g
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
            // [多核] 绝对超时从 1ms 上调到 100ms: 多核 strict/lazy 下 CQ 顺序执行大量
            //   invalid+iofence(每笔~200ns 取指)+每次 iofence 排空, 总仿真时长远超 1ms。
            //   真正的死锁/挂起由 50µs 无进展停滞探测器捕获; 绝对超时仅作兜底。
            if(iommu_md::now_ns()-last_progress_ns>50000 || iommu_md::now_ns()-start_ns>100000000) {
                top.md_trace.diagnostic();
                for(unsigned c=0;c<NCHAN;++c) std::cerr << "[MC_QUEUE] chan=" << c << " queued=" << channels[c].queue.size()
                    << " sent=" << channels[c].injected << '\n';
                std::cerr << "[MC_STATE] responded=" << total_responded
                          << " buffer=" << top.cache_sub.get_pt_dedup_buffer()->get_valid_count()
                          << " groups=" << top.prefetch_groups.size() << " ptw=" << top.ptw_active_walks.size() << '\n';
                std::cerr << "[CQFULL_DIAG] cq=" << top.cq_occupancy()
                          << "/" << top.cq_usable_depth() << " cmds=" << top.cq_submitted_cmds
                          << "/" << top.cq_completed_cmds
                          << " dedup_req=" << top.cache_sub.dedup_request_fifo.num_available()
                          << " dedup_req_free=" << top.cache_sub.dedup_request_fifo.num_free()
                          << " pt_update=" << top.cache_sub.pt_update_fifo.num_available()
                          << " pt_update_free=" << top.cache_sub.pt_update_fifo.num_free()
                          << " dedup_update=" << top.cache_sub.dedup_update_fifo.num_available()
                          << " aggregate_pending=" << top.md_trace.aggregate_pending << '\n';
                require(false,"多通道测试超时，未释放在途payload");
            }
        }
    }
    // ====================================================================
    // [CPU_INVAL] CPU 缓存失效命令发送进程(与请求注入进程完全独立)
    //   数据源: IOMMU 出口记录点(track_4kb_page_out hook)在"同一 4KB 页的
    //     8 个 512B 数据任务全部完成"时刻把 {page_iova, done_ns} 压入
    //     top.cpu_inval_done_pages 并 notify(top.cpu_inval_page_event)。
    //     该时刻即"设备写完 CQE 并发出 MSI"的时刻; INVAL_DELAY_NS 表示软件
    //     响应中断并处理该笔完成的耗时(需求 1-(2): 硬件约 32ns 一笔中断,
    //     软件处理交互慢)。
    //   strict: 每笔完成事件 -> 延时 INVAL_DELAY_NS 后发一条 IOTINVAL.VMA **010**
    //     (GV=0,AV=1,PSCV=0,NL=0,ADDR=页IOVA): 扫描缓存表匹配 addr, 只失效
    //     PT Cache 中匹配的 cacheline(SCAN_RANGE 枚举候选 set); NL=0 => 不动
    //     Walker Cache; AV=1 => 不进 LIB, **不启动延时失效策略**(需求 2-(2))。
    //   lazy:   完成事件只计数, 每满 INVAL_LAZY_N 笔 -> 延时后发一条
    //     IOTINVAL.VMA **100** (GV=1,AV=0,PSCV=0,GSCID=1): 扫描缓存表匹配
    //     GSCID, 清除 PT Cache 与 Walker Cache 中所有 GSCID 匹配的行(本场景
    //     全部行 GSCID=1 => 等价整表清除), 并**启动延时失效策略**(LIB+VN)。
    //   CQ 交互(异步, 规范语义): CPU 写命令入 CQ 内存 + 推进 CQT(门铃)后立即
    //     返回, 由 IOMMU cq_async_process_thread 逐笔取指执行(阻塞于失效流水线)
    //     后推进 CQH。[多核] 每核失效进程 j 只监控 nvme_id==j 的完成事件;
    //     多核共享单一 CQ(深度 8192), 写入 invalid+iofence 两条命令时按仲裁 lock(cq_write_mtx)。
    //   [多核修正] 每发完一笔 invalid, 紧跟一笔 IOFENCE.C(CQ tail +2); IOMMU 执行 iofence 时
    //     停止对在途请求打标并等已打标请求全部排空后才推进 CQH(需求 5-(2))。
    //   [多核修正] 原"CQ 满才反压 iommu 入口"机制去掉: 改为 CPU 失效进程 j 始终反压
    //     sender_j(信用闭环); CQ 深度 8192 理论上不会满, 若满则报错(需求 3-③)。
    // ====================================================================
    // [多核] 每 CPU 核独立统计(数组按核索引); 全局收尾标志 cpu_inval_finalize 单一。
    bool cpu_inval_finalize=false;
    std::array<bool, NCHAN> cpu_inval_finished{};
    std::array<bool, NCHAN> cpu_inval_busy{};        // 已 pop 事件但命令尚未完成(退出条件用)
    std::array<unsigned, NCHAN> cpu_inval_lazy_cnt{};// lazy: 本核未满一批的完成事件计数
    std::array<uint64_t, NCHAN> cpu_inval_cmds{};    // 本核下发的 invalid(+iofence) 笔数
    std::array<uint64_t, NCHAN> cpu_inval_final_cmds{};
    std::array<uint64_t, NCHAN> cpu_inval_dropped_pre{};  // 非测量 epoch 事件丢弃计数
    std::array<uint64_t, NCHAN> cpu_inval_done_events{};  // 测量 epoch 完成事件计数
    std::array<double, NCHAN> cpu_inval_lag_sum{};
    std::array<double, NCHAN> cpu_inval_lag_max{};
    std::array<double, NCHAN> cpu_inval_cqwait_sum{};     // 等待 CQH 越过 invalid+iofence 的时长
    std::array<double, NCHAN> cpu_inval_cqwait_max{};
    double dbg_last_exit_print_ns=-1e9;   // [DBG] 退出条件打印限流
    // [多核] 下发一笔 invalid + 紧跟一笔 IOFENCE.C(CQ tail +2), 等待 IOMMU 顺序执行完这两条
    //   命令(CQ head 越过 iofence), 然后释放本核信用(strict:+1 / lazy:+N 组), 通知 sender_j。
    //   多核共享单一 CQ: 写入两条命令期间持 cq_write_mtx(按仲裁选中某核并 lock)。
    void issue_inval_cmd(unsigned j,uint64_t page,double done_ns,double target_ns,bool is_final) {
        // 等待到目标发起时刻(done+delay); 模拟软件响应 MSI 中断并处理的耗时
        const double now0=iommu_md::now_ns();
        if(target_ns>now0) wait(sc_core::sc_time(target_ns-now0,SC_NS));
        // ---- 原子写入 invalid + iofence 两条命令(多核共享 CQ, 按仲裁 lock) ----
        cq_write_mtx.lock();
        // [CQ 不应满] 深度 8192 >> 稳态在途命令数(<= 2 x cpu_count); 满则报错(需求 3-③)。
        require(top.cq_occupancy()+2<=top.cq_usable_depth(),
                "CQ 队列满(多核信用反压下不应发生): 无法写入 invalid+iofence");
        const uint64_t s_before=top.cq_submitted_cmds;
        const double t_issue=iommu_md::now_ns();
        if(INVAL_LAZY) {
            // 全局失效 100: IOTINVAL.VMA GV=1 AV=0 PSCV=0 GSCID=1 -> 按 GSCID 清 PT+Walker, 启动 LIB/VN
            rp.iotinval_doorbell(&top,VMA,1,0,0,1,0,0);
        } else {
            // 精准失效 010: IOTINVAL.VMA GV=0 AV=1 PSCV=0 NL=0 ADDR=页IOVA -> 只失效 PT 匹配行, 不动 Walker
            rp.iotinval_doorbell(&top,VMA,0,1,0,0,0,page);
        }
        // 紧跟一笔 IOFENCE.C(PR=PW=AV=WSI=0, 纯排空语义): CQ tail +1 => 本笔合计 +2
        rp.iofence_doorbell(&top,IOFENCE_C,0,0,0,0,0,0);
        const double t_post=iommu_md::now_ns();
        cq_write_mtx.unlock();
        // ---- 等待 IOMMU 顺序执行完这两条命令(CQ head 越过 iofence) ----
        const double tw0=iommu_md::now_ns();
        while(!stopped && top.cq_completed_cmds < s_before+2) wait(top.cq_cqh_event);
        const double cqwait=iommu_md::now_ns()-tw0;
        cpu_inval_cqwait_sum[j]+=cqwait; if(cqwait>cpu_inval_cqwait_max[j]) cpu_inval_cqwait_max[j]=cqwait;
        // ---- 释放本核信用: 通知 sender_j 可下发下一批组(strict:1 / lazy:N) ----
        const unsigned batch=INVAL_LAZY?INVAL_LAZY_N:1u;
        cpu_credit[j].credit+=(int)batch;
        cpu_credit[j].groups_released+=batch;
        cpu_credit[j].release_event.notify(SC_ZERO_TIME);
        // ---- 统计 ----
        const double lag=t_issue-(done_ns+INVAL_DELAY_NS);
        cpu_inval_lag_sum[j]+=lag; if(lag>cpu_inval_lag_max[j]) cpu_inval_lag_max[j]=lag;
        const double post=t_post-t_issue;   // 两条命令投递耗时(写CQ内存+CQT)
        ++cpu_inval_cmds[j]; if(is_final) ++cpu_inval_final_cmds[j];
        if(cpu_inval_cmds[j]<=4||cpu_inval_cmds[j]%128==0||is_final) {
            std::cout << "[CPU_INVAL_CMD] cpu=" << j << " #" << cpu_inval_cmds[j]
                      << (is_final?" (final)":"")
                      << (INVAL_LAZY?std::string(" vma100 gscid=1 global"):(" vma010 page="+std::to_string(page)))
                      << " +iofence done=" << done_ns << " issue=" << t_issue
                      << " lag=" << lag << " post=" << post << "ns cqwait=" << cqwait
                      << " cq_occ=" << top.cq_occupancy() << "/" << top.cq_usable_depth()
                      << " submitted=" << top.cq_submitted_cmds
                      << " completed=" << top.cq_completed_cmds
                      << " credit=" << cpu_credit[j].credit << '\n';
            fflush(stdout);
        }
    }
    void cpu_inval_process(unsigned j) {
        std::cout << "[CPU_INVAL] process spawned: cpu=" << j << " mode=" << (INVAL_LAZY?"lazy":"strict")
                  << " delay_ns=" << INVAL_DELAY_NS
                  << (INVAL_LAZY?" lazy_n=":"") << (INVAL_LAZY?std::to_string(INVAL_LAZY_N):std::string())
                  << '\n';
        // 常驻消费循环: 只消费本核(nvme_id==j)的完成事件队列 top.cpu_inval_done_pages[j]。
        //   epoch1 哨兵页等非测量事件丢弃并计数; 仅测量 epoch 内的事件触发失效命令。
        while(!stopped) {
            if(top.cpu_inval_done_pages[j].empty()) {
                if(cpu_inval_finalize) break;
                wait(top.cpu_inval_page_event[j]);
                continue;
            }
            auto rec=top.cpu_inval_done_pages[j].front();
            top.cpu_inval_done_pages[j].pop_front();
            require(rec.nvme_id==j,"完成事件 nvme_id 与 CPU 进程 id 不符(跨核串扰)");
            cpu_inval_busy[j]=true;      // 事件已出队但 invalid+iofence 尚未完成
            if(epoch==iommu_md::MEASUREMENT_EPOCH && !cpu_inval_finalize) {
                ++cpu_inval_done_events[j];
            }
            if(epoch!=iommu_md::MEASUREMENT_EPOCH || cpu_inval_finalize) {
                // 非测量 epoch(epoch1 哨兵/收尾竞态兜底): 不发命令, 只计数丢弃
                ++cpu_inval_dropped_pre[j];
                cpu_inval_busy[j]=false;
                continue;
            }
            if(INVAL_LAZY) {
                // lazy: 完成事件只计数, 满 N 触发一笔 VMA 100 全局失效 + iofence, 释放 N 组信用
                ++cpu_inval_lazy_cnt[j];
                if(cpu_inval_lazy_cnt[j]>=INVAL_LAZY_N) {
                    cpu_inval_lazy_cnt[j]=0;
                    issue_inval_cmd(j,0,rec.done_ns,rec.done_ns+INVAL_DELAY_NS,false);
                }
            } else {
                // strict: 每笔完成 -> 该页 VMA 010 精准失效 + iofence, 释放 1 组信用
                issue_inval_cmd(j,rec.page_iova,rec.done_ns,rec.done_ns+INVAL_DELAY_NS,false);
            }
            cpu_inval_busy[j]=false;
        }
        // 收尾: lazy 残余计数(<N)强制补发一笔; strict 队列已在上面排空
        if(INVAL_LAZY && cpu_inval_lazy_cnt[j]>0 && !stopped) {
            const double done=iommu_md::now_ns();
            cpu_inval_busy[j]=true;
            issue_inval_cmd(j,0,done,done,true);
            cpu_inval_busy[j]=false;
            cpu_inval_lazy_cnt[j]=0;
        }
        cpu_inval_finished[j]=true;
        std::cout << "[CPU_INVAL] process finished: cpu=" << j << " cmds=" << cpu_inval_cmds[j] << '\n';
    }
    void print_cpu_inval_report() const {
        printf("\n========== CPU Cache Invalidation Statistics (multi-core) ==========\n");
        printf("  Mode:                  %s\n", INVAL_LAZY ? "lazy (VMA 100: GSCID global, delayed-inval strategy)"
                                                           : "strict (VMA 010: addr precise, PT only)");
        printf("  CPU count:             %u\n", NCHAN);
        printf("  Inval delay (SW react):%.0f ns\n", INVAL_DELAY_NS);
        if(INVAL_LAZY) printf("  Lazy N:                %u pages/cmd\n", INVAL_LAZY_N);
        printf("  4KB done events total: %lu (IOMMU track_4kb_page_out)\n",
               (unsigned long)top.cpu_inval_4kb_done_total);
        uint64_t cmds=0,finals=0,drops=0,events=0; double lag_sum=0,lag_max=0,cw_sum=0,cw_max=0;
        for(unsigned j=0;j<NCHAN;++j) {
            cmds+=cpu_inval_cmds[j]; finals+=cpu_inval_final_cmds[j];
            drops+=cpu_inval_dropped_pre[j]; events+=cpu_inval_done_events[j];
            lag_sum+=cpu_inval_lag_sum[j]; if(cpu_inval_lag_max[j]>lag_max) lag_max=cpu_inval_lag_max[j];
            cw_sum+=cpu_inval_cqwait_sum[j]; if(cpu_inval_cqwait_max[j]>cw_max) cw_max=cpu_inval_cqwait_max[j];
            printf("  [cpu %u] done=%lu events=%lu cmds=%lu(final=%lu) drops=%lu lag_avg=%.1f cqwait_avg=%.1f cqwait_max=%.1f\n",
                   j,(unsigned long)top.cpu_inval_4kb_done_per_cpu[j],(unsigned long)cpu_inval_done_events[j],
                   (unsigned long)cpu_inval_cmds[j],(unsigned long)cpu_inval_final_cmds[j],
                   (unsigned long)cpu_inval_dropped_pre[j],
                   cpu_inval_cmds[j]?cpu_inval_lag_sum[j]/cpu_inval_cmds[j]:0.0,
                   cpu_inval_cmds[j]?cpu_inval_cqwait_sum[j]/cpu_inval_cmds[j]:0.0,
                   cpu_inval_cqwait_max[j]);
        }
        printf("  ---- aggregate ----\n");
        printf("  Meas done events:      %lu   Non-meas drops: %lu\n",(unsigned long)events,(unsigned long)drops);
        printf("  Inval cmds issued:     %lu (final-drain=%lu)  [each + 1 iofence => CQ tail +2/cmd]\n",
               (unsigned long)cmds,(unsigned long)finals);
        if(cmds>0) {
            printf("  Issue lag avg:         %.1f ns max=%.1f ns\n",lag_sum/cmds,lag_max);
            printf("  CQH wait avg:          %.1f ns max=%.1f ns (invalid+iofence 完成等待)\n",cw_sum/cmds,cw_max);
        }
        printf("  IOFENCE executed:      %lu  drain avg=%.1f ns max=%.1f ns  inflight_now=%d\n",
               (unsigned long)top.iofence_exec_count,
               top.iofence_exec_count?top.iofence_drain_ns_sum/top.iofence_exec_count:0.0,
               top.iofence_drain_ns_max, top.iofence_inflight_count);
        printf("====================================================================\n");
        fflush(stdout);
    }
    // [多核信用反压] 每核 sender 反压统计 + CQ 占用观测(替代原 CQ-full-only 门控报告)。
    void print_inject_credit_report() const {
        std::cout << "[INVAL_CPU_CREDIT] mode=" << (INVAL_LAZY?"lazy":"strict")
                  << " policy=PER_CPU_CREDIT cpu_count=" << NCHAN
                  << " init_credit=" << (INVAL_LAZY?INVAL_LAZY_N:1u)
                  << " cq_peak=" << top.cq_occ_peak << "/" << top.cq_usable_depth()
                  << " cq_submitted=" << top.cq_submitted_cmds
                  << " cq_completed=" << top.cq_completed_cmds << '\n';
        uint64_t tot_sent=0,tot_rel=0,tot_blk=0; double tot_blk_ns=0,max_blk=0;
        for(unsigned j=0;j<NCHAN;++j) {
            const auto& cc=cpu_credit[j];
            tot_sent+=cc.groups_sent; tot_rel+=cc.groups_released; tot_blk+=cc.block_count;
            tot_blk_ns+=cc.blocked_ns; if(cc.blocked_max_ns>max_blk) max_blk=cc.blocked_max_ns;
            std::cout << "[INVAL_CPU_CREDIT] cpu=" << j
                      << " groups_sent=" << cc.groups_sent
                      << " groups_released=" << cc.groups_released
                      << " credit_left=" << cc.credit
                      << " block_count=" << cc.block_count
                      << " blocked_ns=" << cc.blocked_ns
                      << " blocked_max_ns=" << cc.blocked_max_ns << '\n';
        }
        const double win=(top.cq_win_start_ns>=0.0 && top.cq_win_end_ns>top.cq_win_start_ns)
                         ? (top.cq_win_end_ns-top.cq_win_start_ns) : 0.0;
        std::cout << "[INVAL_CPU_CREDIT_TOT] groups_sent=" << tot_sent
                  << " groups_released=" << tot_rel
                  << " block_count=" << tot_blk
                  << " blocked_ns=" << tot_blk_ns << " blocked_max_ns=" << max_blk
                  << " sender_backpressure_ratio=" << (win>0.0?100.0*tot_blk_ns/(win*NCHAN):0.0) << "%\n";
        // 稳态并发组数理论值: strict=cpu_count / lazy=cpu_count x N
        std::cout << "[INVAL_CONC_THEORY] steady concurrent NVMe groups = "
                  << (INVAL_LAZY?(NCHAN*INVAL_LAZY_N):NCHAN)
                  << " (cpu_count x " << (INVAL_LAZY?std::to_string(INVAL_LAZY_N):std::string("1")) << ")\n";
        fflush(stdout);
    }
    void run_epoch(unsigned e) {
        for(auto& c:channels) {
            require(c.queue.empty(),"切换epoch时队列非空");
            c.ready=c.done=false; c.injected=c.scheduled=c.peak_queue=c.full_waits=0; c.full_wait_ns=0;
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
#if TEST_CFG_CPU_INVAL_SCENE
        // [CPU_INVAL] 切换 epoch 前清空每核完成事件队列(epoch1/2 无 512B 数据任务, 正常为空;
        //   此处兜底清空避免跨 epoch 残留事件被误消费)
        {
            size_t residual=0;
            for(unsigned j=0;j<NCHAN;++j){ residual+=top.cpu_inval_done_pages[j].size(); top.cpu_inval_done_pages[j].clear(); }
            if(residual>0) std::cout << "[CPU_INVAL] epoch=" << e << " 残留完成事件 " << residual << " 笔, 已清空\n";
        }
        // [多核信用反压] 测量 epoch: 每核建立初始注入信用(strict:1 组 / lazy:N 组)并打开统计窗口。
        //   非测量 epoch 不参与门控(producer 只在测量 epoch 读信用)。
        if(e==iommu_md::MEASUREMENT_EPOCH) {
            require(top.cq_submitted_cmds==top.cq_completed_cmds,"进入测量epoch时CQ未排空");
            const int init_credit=INVAL_LAZY?(int)INVAL_LAZY_N:1;
            for(unsigned j=0;j<NCHAN;++j) {
                // 注: CpuCredit 含 sc_event(不可赋值), 逐字段重置而非整体赋值。
                cpu_credit[j].credit=init_credit;   // 初始信用: sender_j 只能先下发这么多组
                cpu_credit[j].groups_sent=0; cpu_credit[j].groups_released=0;
                cpu_credit[j].block_count=0; cpu_credit[j].blocked_ns=0.0; cpu_credit[j].blocked_max_ns=0.0;
                cpu_inval_lazy_cnt[j]=0; cpu_inval_cmds[j]=0; cpu_inval_final_cmds[j]=0;
                cpu_inval_dropped_pre[j]=0; cpu_inval_done_events[j]=0;
                cpu_inval_lag_sum[j]=0.0; cpu_inval_lag_max[j]=0.0;
                cpu_inval_cqwait_sum[j]=0.0; cpu_inval_cqwait_max[j]=0.0;
                cpu_inval_busy[j]=false; cpu_inval_finished[j]=false;
            }
            top.cq_win_start_ns=iommu_md::now_ns();
            // 占用/满 时间加权采样重新定基, 使两项占比只覆盖测量窗口
            top.cq_occ_peak=0; top.cq_occ_area_ns=0.0; top.cq_full_area_ns=0.0;
            top.cq_occ_last_ns=iommu_md::now_ns(); top.cq_occ_cur=0;
            std::cout << "[INVAL_CREDIT] epoch=" << e << " per-cpu initial credit=" << init_credit
                      << " (strict:1 / lazy:N=" << INVAL_LAZY_N << ") cpu_count=" << NCHAN
                      << " cq_usable=" << top.cq_usable_depth() << "\n";
        }
#endif
        phase_event.notify(SC_ZERO_TIME);
        while(true) {
            bool ok=sent && total_responded==total_count(e) && top.md_quiescent();
#if TEST_CFG_CPU_INVAL_SCENE
            // [多核] 追加退出条件: 所有核完成事件队列已排空 + 无核持有未完成的 invalid+iofence
            //   + CQ 已排空(全部已投递命令都被 IOMMU 执行完并推进了 CQH)。
            {
                size_t pending=0; bool any_busy=false;
                for(unsigned j=0;j<NCHAN;++j){ pending+=top.cpu_inval_done_pages[j].size(); any_busy=any_busy||cpu_inval_busy[j]; }
                ok=ok && pending==0 && !any_busy && (top.cq_submitted_cmds==top.cq_completed_cmds);
            }
#endif
            if(ok) break;
#if TEST_CFG_CPU_INVAL_SCENE
            // [DBG] 响应齐全后仍未退出 -> 每秒打印一次阻塞条件
            if(sent && total_responded==total_count(e)) {
                if(iommu_md::now_ns()-dbg_last_exit_print_ns>1000.0) {
                    dbg_last_exit_print_ns=iommu_md::now_ns();
                    size_t pending=0; unsigned busy_cnt=0;
                    for(unsigned j=0;j<NCHAN;++j){ pending+=top.cpu_inval_done_pages[j].size(); if(cpu_inval_busy[j]) ++busy_cnt; }
                    std::cout << "[EXIT_DBG] epoch=" << e << " t=" << iommu_md::now_ns()
                              << " quiescent=" << top.md_quiescent()
                              << " done_pages=" << pending << " busy_cpus=" << busy_cnt
                              << " cq_sub=" << top.cq_submitted_cmds
                              << " cq_done=" << top.cq_completed_cmds
                              << " cq_occ=" << top.cq_occupancy() << '\n';
                    fflush(stdout);
                }
            }
#endif
            wait(1,SC_NS);
        }
#if TEST_CFG_CPU_INVAL_SCENE
        if(e==iommu_md::MEASUREMENT_EPOCH) top.cq_win_end_ns=iommu_md::now_ns();
#endif
        uint64_t total_injected=0;
        for(unsigned c=0;c<NCHAN;++c) {
            require(channels[c].injected==chan_count(c,e),"发送/完成数量不一致");
            total_injected+=channels[c].injected;
            std::cout << "[MC_RP] epoch=" << e << " chan=" << c << " injected=" << channels[c].injected
                      << " queue_peak=" << channels[c].peak_queue
                      << " full_waits=" << channels[c].full_waits << " full_wait_ns=" << channels[c].full_wait_ns << '\n';
        }
        require(total_responded==total_injected,"响应总数与注入总数不一致");
    }
public:
    explicit MultiChannelScenario(RP_Module& r) : rp(r),top(*r.iommu_ptr) {}
    ~MultiChannelScenario() {
        // 正常结束只在所有响应及异步工作排空后清理；异常停止不提前清理。
        for(auto& trans:payloads) {
            PayloadExtention* ext=nullptr; trans->get_extension(ext);
            trans->clear_extension(ext); delete ext;
            delete[] trans->get_data_ptr(); trans->set_data_ptr(nullptr);
        }
    }
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext=nullptr; trans.get_extension(ext);
        require(ext && ext->requester_id<1,"响应缺少有效设备ID");
        auto& r=top.md_trace.request(&trans);
        require(r.epoch==epoch,"收到旧epoch响应");
        top.md_trace.response(trans,0);   // 单一设备聚合(did=0)。
        ++total_responded;
    }
    void run() {
        wait(20,SC_NS);
        start_ns=last_progress_ns=iommu_md::now_ns();
        std::cout << "[MC_CONFIG] n=" << NCHAN << " IOVA_MB=" << RANGE_MB
                  << " groups_base=" << GROUPS_BASE << " groups_rem=" << GROUPS_REM
                  << " queue=" << QUEUE_DEPTH << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " rr_start=" << RR_START << " load_mode=" << (FIXED_TOTAL ? "fixed_total" : "per_channel")
                  << " total_meas_pkts=" << total_count(iommu_md::MEASUREMENT_EPOCH) << '\n';
        std::cout << "[MC_INVAL_CONFIG] mode=" << (INVAL_LAZY?"lazy":"strict")
                  << " cmd=" << (INVAL_LAZY?"IOTINVAL.VMA 100 (GV=1,AV=0,PSCV=0,GSCID=1)"
                                           :"IOTINVAL.VMA 010 (GV=0,AV=1,PSCV=0,NL=0,ADDR)")
                  << " + IOFENCE.C(每笔 invalid 后紧跟 => CQ tail +2)"
                  << " cpu_count=" << NCHAN
                  << " cq_depth=" << INVAL_CQ_DEPTH
                  << " sw_delay_ns=" << INVAL_DELAY_NS
                  << (INVAL_LAZY?" lazy_n=":"") << (INVAL_LAZY?std::to_string(INVAL_LAZY_N):std::string())
                  << " init_credit=" << (INVAL_LAZY?std::to_string(INVAL_LAZY_N):std::string("1"))
                  << " group(s)/cpu  policy=PER_CPU_CREDIT"
                  << " steady_concurrent_groups=" << (INVAL_LAZY?(NCHAN*INVAL_LAZY_N):NCHAN) << '\n';
        setup();
        for(unsigned c=0;c<NCHAN;++c) sc_spawn([this,c](){ producer(c); });
        sc_spawn([this](){ scheduler(); });
#if TEST_CFG_SPLIT_RW_PORTS
        sc_spawn([this](){ sender(false); });   // sender_r: 读口发送进程(常驻, 每 epoch 不重复 spawn)
        sc_spawn([this](){ sender(true); });    // sender_w: 写口发送进程
#endif
        sc_spawn([this](){ watchdog(); });
        // [CPU_INVAL 多核] cpu_count 个 CPU 缓存失效命令发送进程: 各与对应请求发送进程独立,
        //   每核只监控自己 nvme_id 的完成事件, 常驻, 仅在测量 epoch 与收尾阶段活动。
        for(unsigned j=0;j<NCHAN;++j) sc_spawn([this,j](){ cpu_inval_process(j); });
        invalidate_all();
        run_epoch(1); // 哨兵: 打通翻译/响应路由(单设备, 全通道同结果)。
        invalidate_all();
        run_epoch(2); // 预热不进入正式统计(MSIPT + SQ/CQ PT Cache)。
        // [STAT口径对齐] 与 multidev/multidev_mc 一致: 预热结束后 IOMMU 内部 quiescent,
        //   测量 epoch 前重置 4KB 并发窗口与去重 Cache 有效行采样累加器, 使两项指标只覆盖测量窗口。
        top.reset_4kb_concurrency_window();
        top.cache_sub.reset_dedup_valid_line_stats();
#if TEST_CFG_CPU_INVAL_SCENE
        // [CPU_INVAL] 兜底清空 epoch1/2 期间每核的完成事件(正常为空), 保证统计只覆盖测量窗口
        for(unsigned j=0;j<NCHAN;++j) top.cpu_inval_done_pages[j].clear();
#endif
        // [STAT] 打开 IOMMU 侧稳态 IOPS/Theory peak/Efficiency 采样窗口(与 mps128 项同口径),
        //   窗口按"已完成数(epoch1+2) + 正式epoch的10%~90%"精确对齐。
        {
            const uint64_t done_before=top.iommu_total_completed;
            const uint64_t meas=total_count(iommu_md::MEASUREMENT_EPOCH);
            top.steady_start_count=done_before+meas*STEADY_STATE_START_PERCENT/100;
            top.steady_end_count  =done_before+meas*STEADY_STATE_END_PERCENT/100;
            std::cout << "[MC_STAT_WINDOW] done_before=" << done_before
                      << " steady_start_count=" << top.steady_start_count
                      << " steady_end_count=" << top.steady_end_count << '\n';
        }
        const uint64_t ptw_before=top.ptw_total_completed, ddr_before=top.axi_master_1_read_req_total;
        run_epoch(iommu_md::MEASUREMENT_EPOCH);
        // [CPU_INVAL] 收尾: 通知 CPU 失效进程排空残余事件(strict 队列/lazy 计数<N),
        //   等待其退出后再出报告, 保证 cmds 统计完整且失效流水线无在途任务。
        cpu_inval_finalize=true;
        for(unsigned j=0;j<NCHAN;++j) top.cpu_inval_page_event[j].notify(SC_ZERO_TIME);
        {
            const double t0=iommu_md::now_ns();
            bool all_done=false;
            while(!all_done && iommu_md::now_ns()-t0<500000.0) {
                all_done=true; for(unsigned j=0;j<NCHAN;++j) all_done=all_done&&cpu_inval_finished[j];
                if(!all_done) wait(10,SC_NS);
            }
            require(all_done,"CPU失效进程收尾超时(500us)");
        }
        // [CQ反压] 收尾: 等待 IOMMU 把 CQ 中残余命令全部执行完(CQH 追上 CQT)。
        //   run_epoch 的退出条件已含该项, 此处为 lazy 收尾补发命令的兜底。
        {
            const double t0=iommu_md::now_ns();
            while(top.cq_submitted_cmds!=top.cq_completed_cmds && iommu_md::now_ns()-t0<500000.0)
                wait(10,SC_NS);
            require(top.cq_submitted_cmds==top.cq_completed_cmds,"CQ 收尾排空超时(500us)");
            if(top.cq_win_end_ns<top.cq_last_done_ns) top.cq_win_end_ns=top.cq_last_done_ns;
        }
        print_cpu_inval_report();
        print_inject_credit_report();
        top.print_cq_async_report();
        // [CPU_INVAL] 功能自检:
        //   (1) 完成事件数应等于测量窗口内出现过的 distinct 4KB 页数
        //       (页重叠时同一页的多组任务合并计数, 只在最后一个任务完成时记一次);
        //   (2) strict 命令数=事件数; lazy 命令数=ceil(事件数/N);
        //   (3) CQ 全部命令执行完毕(submitted==completed), 且占用峰值 <= 可用深度;
        //   (4) 信用守恒: 放行组数 == 初始信用 + CQH 推进次数 - 期末余额;
        //   (5) strict 不进 LIB(延时失效策略未启动); lazy 每笔命令进 LIB 一次;
        //   (6) 投递耗时 ~0(证明命令投递与执行已解耦, 非同步退化)。
        {
            // [多核] 逐核校验完成事件/命令数/信用守恒, 再聚合校验 CQ/iofence/失效语义。
            const uint64_t done=top.cpu_inval_4kb_done_total;
            uint64_t meas_events=0, drops=0, cmds=0, groups=0;
            for(unsigned j=0;j<NCHAN;++j) {
                const uint64_t ev=cpu_inval_done_events[j];
                const uint64_t gj=chan_groups(j);           // 本核 NVMe 组数(per_channel: =NUM_PAGES)
                const uint64_t expect_j=INVAL_LAZY?(ev+INVAL_LAZY_N-1)/INVAL_LAZY_N:ev;
                meas_events+=ev; drops+=cpu_inval_dropped_pre[j]; cmds+=cpu_inval_cmds[j]; groups+=gj;
                std::cout << "[CPU_INVAL_CHECK] cpu=" << j
                          << " done_per_cpu=" << top.cpu_inval_4kb_done_per_cpu[j]
                          << " meas_events=" << ev << " groups=" << gj
                          << " expect_cmds=" << expect_j << " issued=" << cpu_inval_cmds[j]
                          << " match=" << (cpu_inval_cmds[j]==expect_j?"PASS":"FAIL")
                          << " groups_sent=" << cpu_credit[j].groups_sent
                          << " groups_released=" << cpu_credit[j].groups_released
                          << " credit_left=" << cpu_credit[j].credit
                          << " block_count=" << cpu_credit[j].block_count
                          << " blocked_ns=" << cpu_credit[j].blocked_ns << '\n';
                // 每核: 完成事件数 == 本核组数(每组=一个4KB页完成); 命令数 == 预期; 放行组数 == 本核组数
                require(ev==gj,"本核4KB完成事件数与组数不符(nvme_id 归属错误?)");
                require(cpu_inval_cmds[j]==expect_j,"本核失效命令数与预期不符");
                require(cpu_credit[j].groups_sent==gj,"本核放行组数与NVMe组数不符");
            }
            uint64_t distinct=0,out_no_match=0;
            for(const auto& kv:top.kb4_dev_stats){ distinct+=kv.second.distinct_pages; out_no_match+=kv.second.out_no_match; }
            const uint64_t lib_rec=top.cache_sub.lazy_record_count();
            const uint64_t lib_wrap=top.cache_sub.lazy_wrap_count();
            const uint64_t lib_full=top.cache_sub.lazy_full_fallback();
            std::cout << "[CPU_INVAL_CHECK_TOT] cpu_count=" << NCHAN
                      << " mode=" << (INVAL_LAZY?"lazy":"strict")
                      << " done_events=" << done << " meas_events=" << meas_events
                      << " drops=" << drops << " groups=" << groups
                      << " cmds=" << cmds << " distinct_pages=" << distinct
                      << " out_no_match=" << out_no_match
                      << " cq_submitted=" << top.cq_submitted_cmds
                      << " cq_completed=" << top.cq_completed_cmds
                      << " cq_occ_peak=" << top.cq_occ_peak << "/" << top.cq_usable_depth()
                      << " iofence_exec=" << top.iofence_exec_count
                      << " lib_records=" << lib_rec << " lib_wraps=" << lib_wrap
                      << " lib_full_fallback=" << lib_full
                      << " pt_lazy_drops=" << top.cache_sub.pt_lazy_inval_drops()
                      << " walker_lazy_drops=" << top.cache_sub.walker_lazy_inval_drops() << '\n';
            require(meas_events>0,"测量epoch内4KB完成事件数为0, 记录点未生效");
            require(out_no_match==0,"4KB出口存在 out_no_match 异常");
            // ---- CQ 异步 + iofence 自检 ----
            // 每笔 invalid 后跟一笔 iofence => CQ 投递命令数 = 2 x invalid 笔数(CQ tail +2)
            require(top.cq_submitted_cmds==2*cmds,"CQ投递数应=2x invalid笔数(invalid+iofence)");
            require(top.cq_completed_cmds==top.cq_submitted_cmds,"CQ未全部执行完毕");
            require(top.iofence_exec_count==cmds,"iofence执行笔数应=invalid笔数");
            // [需求3-③/⑥] CQ 从不满: 占用峰值必须 < 可用深度(满则视为错误)
            require(top.cq_occ_peak<top.cq_usable_depth(),"CQ队列满(多核信用反压下不应发生)");
            // ---- 失效语义自检 ----
            if(INVAL_LAZY) {
                require(lib_rec==cmds,"lazy: 每笔 VMA 100 都应进入 LIB(延时失效策略)");
                require(top.cache_sub.pt_lazy_inval_drops()+top.cache_sub.walker_lazy_inval_drops()>0,
                        "lazy: 延时失效未在查询旁路生效(drops=0)");
            } else {
                require(lib_rec==0,"strict: VMA 010 精准失效不得启动延时失效策略(LIB 记录应为0)");
                require(lib_wrap==0 && lib_full==0,"strict: LIB 回绕/降级计数应为0");
            }
        }
#if TEST_CFG_SPLIT_RW_PORTS
        std::cout << "[MC_RWQ] depth=" << RWQ_DEPTH << " peak_read=" << rw_peak_r
                  << " peak_write=" << rw_peak_w << '\n';
        // [供给验证] 注入期内读/写发送进程的饥饿次数与时长:
        //   starve_ns 越接近 0 => RP 侧请求越充足(端口从不因缺请求而空等)。
        std::cout << "[MC_SUPPLY] R starve_cnt=" << starve_cnt_r
                  << " starve_ns=" << starve_ns_r << " last_send=" << last_send_ns_r
                  << " | W starve_cnt=" << starve_cnt_w
                  << " starve_ns=" << starve_ns_w << " last_send=" << last_send_ns_w << '\n';
        // 报告前缀按失效模式+CPU核数隔离(mc_inval_{strict|lazy_N}_cpu{N}), 与基线 CSV 完全隔离。
        top.md_trace.report(std::string("mc_inval_")
                            +(INVAL_LAZY?("lazy_N"+std::to_string(INVAL_LAZY_N)):"strict")
                            +"_cpu"+std::to_string(NCHAN)+"_"+std::to_string(RANGE_MB)+"mb");
#else
        top.md_trace.report(std::string("mc_inval_")
                            +(INVAL_LAZY?("lazy_N"+std::to_string(INVAL_LAZY_N)):"strict")
                            +"_cpu"+std::to_string(NCHAN)+"_"+std::to_string(RANGE_MB)+"mb");
#endif
        std::cout << "[MC_DELTA] PTW=" << top.ptw_total_completed-ptw_before
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
        stopped=true; phase_event.notify(SC_ZERO_TIME); ready_event.notify(SC_ZERO_TIME);
        for(auto& c:channels) c.space_event.notify(SC_ZERO_TIME);
        // [多核信用反压] 兜底唤醒可能停在信用门控里的 producer(正常收尾时它已退出循环)
        for(unsigned j=0;j<NCHAN;++j) cpu_credit[j].release_event.notify(SC_ZERO_TIME);
        top.cq_cqh_event.notify(SC_ZERO_TIME);
        uint64_t tot_cmds=0, tot_blk=0;
        for(unsigned j=0;j<NCHAN;++j){ tot_cmds+=cpu_inval_cmds[j]; tot_blk+=cpu_credit[j].block_count; }
        std::cout << (INVAL_LAZY?"[MC_INVAL_LAZY_RESULT]":"[MC_INVAL_STRICT_RESULT]")
                  << " PASS expected=" << total_count(iommu_md::MEASUREMENT_EPOCH)
                  << " completed=" << total_responded
                  << " cpu_count=" << NCHAN
                  << " cq_depth=" << INVAL_CQ_DEPTH
                  << (INVAL_LAZY?" lazy_n=":"") << (INVAL_LAZY?std::to_string(INVAL_LAZY_N):std::string())
                  << " cmds=" << tot_cmds << " iofence=" << top.iofence_exec_count
                  << " cq_occ_peak=" << top.cq_occ_peak << "/" << top.cq_usable_depth()
                  << " sender_blocks=" << tot_blk << "\n";
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario=std::make_shared<MultiChannelScenario>(*this);
    md_response_handler=[scenario](tlm_generic_payload& trans){ scenario->response(trans); };
    scenario->run();
}
#endif
