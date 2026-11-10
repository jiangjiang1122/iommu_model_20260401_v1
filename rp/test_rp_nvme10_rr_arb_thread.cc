#include "test_rp.hh"
#include "iommu_top.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

// ============================================================================
// [激励重构 20261103] NVMe 任务队列 + 嵌套轮询仲裁场景 (nvme10_rr)
//
// 与旧 rand4k_nvme10_multidev_mc_*(逐包 2D 轮询 i=c*N+d + 单一 scheduler +
//   中间读写发送队列)的唯一区别是 **RP 侧激励组织方式**; IOMMU 逻辑、页表/地址
//   布局、并发额度、Cache 配置与统计口径逐宏一致, 便于与场景1(8盘实测回放)对比。
//
// 设备内(需求1~4):
//   - 每设备 C 个 NVMe 任务生成进程 generator(d,c), 每个进程独占绑定一组
//     (读口队列 c, 写口队列 c) 并严格按"1 读任务 + 1 写任务"交替生成;
//     => 反压按通道隔离: 任一队列满/空只影响本通道, 不会把写口准入阻塞传导成
//        读口缺任务(旧"每设备 1 个生成进程"结构的实测缺陷: QDEPTH=8 时读发送
//        进程饥饿 109 次/12.85us)。
//   - 1 个 NVMe 任务 = 10 包, 顺序固定: SQ(读,64B) -> 8x512B 数据 -> CQ(写,16B),
//     本场景不含 MSI(建表 MSIPTP_Off);
//   - C 组读口队列 + C 组写口队列(C = 每设备通道数, 1 通道 = 1 读口队列 + 1 写口队列)。
//     任务按方向拆成两个"方向突发":
//       读向突发 = 读任务[SQ,D0..D7](9包) / 写任务[SQ](1包)
//       写向突发 = 读任务[CQ](1包)        / 写任务[D0..D7,CQ](9包)
//     派发规则(同索引): 通道 c 的任务流, 两个方向突发都进入 **同索引 c** 的读口/
//     写口队列 => 每个队列都携带读写混合任务(与真实 NVMe 队列对语义一致),
//     且各队列突发数/包数精确相等(读写各半: 9+1 与 1+9)。
//     每队列默认无界 QDEPTH(=TASKS+8) => 生成进程整轮零阻塞 => 构造性饱和
//     (A0: 任何仲裁访问必命中就绪任务, 数学上不可能空选)。
//
// 设备间(需求1~5):
//   - 全系统只有 2 个发送进程(与 N、C 无关): sender_r 独占读口 socket、sender_w
//     独占写口 socket, 进程内直接 nb_transport_fw(端口节拍 + 320/320/640 准入在
//     回调内阻塞本方向), 读写互不影响; 无中间队列 => 注入顺序 == 仲裁顺序;
//   - 嵌套轮询: 外层设备 RR、内层"设备内队列"RR, 每步只出队 **1 个包** 后切换设备,
//     设备内队列游标每步 +1(走完最后一个队列后回到第 1 个)。
//     C=2 时访问序: 1盘q0 -> 2盘q0 -> ... -> 8盘q0 -> 1盘q1 -> ... -> 8盘q1 -> 1盘q0;
//   - 目的: 设备内读写完全打散, 同方向多队列之间完全打散。
//
// 负载口径 per_queue(默认): 每个(读口队列,写口队列)队列对自带 TASKS 个任务 /
//   TASKS 个全局唯一 4KB 页 => 每设备任务数与页数随 C 线性翻倍
//   (C=1: 250 任务/250 页/2500 包每方向; C=2: 500 任务/500 页/5000 包每方向;
//   N=8 时 C=1 全局 20000 包, C=2 全局 40000 包)。这是"4KB 页并发随队列组数增长"
//   的前提; fixed_total 旧口径(每设备恒 TASKS 任务)只保留给扩展性对照矩阵。
//
// 本文件仅在 TEST=rand4k_nvme10_rr_n8c1 / _n8c2 时经 TEST_THREAD_SRC 编入。
// ============================================================================

#ifndef TEST_CFG_NVME10_RR_SCENE
#define TEST_CFG_NVME10_RR_SCENE 0
#endif
#ifndef TEST_CFG_NUM_DEVICES
#define TEST_CFG_NUM_DEVICES 8
#endif
#ifndef TEST_CFG_MD_CHAN_PER_DEV
#define TEST_CFG_MD_CHAN_PER_DEV 1
#endif
#ifndef TEST_CFG_NVRR_TASKS_PER_DEV
#define TEST_CFG_NVRR_TASKS_PER_DEV 250
#endif
#ifndef TEST_CFG_NVRR_QUEUE_DEPTH
#define TEST_CFG_NVRR_QUEUE_DEPTH 0
#endif
#ifndef TEST_CFG_NVRR_RR_START
#define TEST_CFG_NVRR_RR_START 0
#endif
#ifndef TEST_CFG_NVRR_PER_QUEUE
#define TEST_CFG_NVRR_PER_QUEUE 1
#endif

#if TEST_CFG_NVME10_RR_SCENE
#if !TEST_CFG_MULTI_DEVICE_SCENE
#error "TEST_CFG_NVME10_RR_SCENE 需要 TEST_CFG_MULTI_DEVICE_SCENE=1 (复用 md_trace)"
#endif
#if !TEST_CFG_SPLIT_RW_PORTS
#error "TEST_CFG_NVME10_RR_SCENE 需要 TEST_CFG_SPLIT_RW_PORTS=1 (每端口独占 1 个发送进程)"
#endif
#ifndef TEST_CFG_S13_IOVA_512MB
#error "TEST_CFG_NVME10_RR_SCENE 需要 TEST_CFG_S13_IOVA_512MB (与场景1/旧场景2-3 同 IOVA 布局)"
#endif

namespace {
using iommu_md::require;

// ------------------------------ 场景参数 ------------------------------
constexpr unsigned N     = TEST_CFG_NUM_DEVICES;          // 设备数
constexpr unsigned C     = TEST_CFG_MD_CHAN_PER_DEV;      // 每设备读/写口队列组数
constexpr unsigned TASKS = TEST_CFG_NVRR_TASKS_PER_DEV;   // 每队列对的 NVMe 任务数(读写交替)
// [负载口径 20261104] PER_QUEUE=1: 每个(读口队列,写口队列)队列对自带一条独立任务流
//   (TASKS 个任务 / TASKS 个全局唯一页) => 每设备任务数与页数随 C 线性翻倍
//   (C=2 → 500 任务/500 页/5000 包每设备), 这是"4KB 页并发随队列组数增长"的前提;
//   PER_QUEUE=0(fixed_total): 每设备恒 TASKS 任务, C 只把同一条流切给 C 个队列对
//   (旧口径, 仅用于扩展性对照矩阵)。
constexpr bool     PER_QUEUE  = (TEST_CFG_NVRR_PER_QUEUE != 0);
constexpr unsigned TASKS_DEV  = PER_QUEUE ? TASKS * C : TASKS;   // 每设备任务总数
constexpr unsigned PAGES_DEV  = TASKS_DEV;                       // 每设备全局唯一页数(每任务1页)
// 每队列突发深度: 0 = 自动(TASKS+8), 即生成进程整轮不阻塞 => 供给恒充足(需求4)。
//   [为何默认无界] 每个生成进程 (d,c) 每任务要同时向自己的读口/写口队列各投 1 个
//   突发, 若队列浅, 一侧端口因并发准入阻塞会让生成进程卡在另一侧队列上, 从而把
//   "写口反压"传导成"读口缺任务"(实测 QDEPTH=8 时读发送进程饥饿 109 次/12.85us),
//   违背需求"读写各自独立、互不影响"与"不会出现仲裁到某队列时没有就绪任务"。
//   场景1(实测回放)也是把全部记录载入内存后无界供给, 故本默认与场景1 同口径。
//   每队列最多收 TASKS 个突发 < TASKS+8 => 构造性零阻塞(gen_block_ns==0)。
//   显式传较小值可重新启用反压, 供灵敏度对比; 该模式下降级为"经验饱和",
//   A0 硬门禁不适用, 仅报告不门禁。
constexpr bool     UNBOUNDED = (TEST_CFG_NVRR_QUEUE_DEPTH == 0);
constexpr unsigned QDEPTH= UNBOUNDED
                           ? (TASKS + 8) : unsigned(TEST_CFG_NVRR_QUEUE_DEPTH);
constexpr unsigned RR_START = TEST_CFG_NVRR_RR_START;     // 设备轮询起点
constexpr unsigned WARMUP_TASKS  = 8;                     // 预热: 8 伪任务(SQ+CQ) = 16 包/设备
constexpr unsigned SENTINEL_PKTS = 2;                     // 哨兵: 2 个 PROBE 读包/设备

// ------------------------------ 地址布局(512MB, 与旧场景同值) ------------------------------
constexpr uint64_t IOVA_BASE  = 0x100000ULL;
constexpr unsigned RANGE_MB   = 512, HUGE_COUNT = 50;
constexpr unsigned RANGE_PAGES= RANGE_MB * 256;                       // 131072 个 4KB 页
// [边界安全] 预留 PT_DEDUP_PREFETCH_DEPTH 个页, 使任何被选页的预取邻居都已建表。
constexpr unsigned PAGE_PICK_RANGE = RANGE_PAGES - PT_DEDUP_PREFETCH_DEPTH;

// ============ [控制包散布 20261105] SQ/CQ 不再共用单一页 ============
// 背景: 实测场景1 回放中, 64B 小读(SQ 语义)自然落在 45 个不同 4KB 页上
//   (1972 包 / 45 页 = 43.8 包每页), 16B 控制写(CQ 语义)落在 24 个页上
//   (2021 包 / 24 页 = 84.2 包每页)。而本场景原实现把所有 SQ 包打到同一个
//   SQ_IOVA、所有 CQ 包打到同一个 CQ_IOVA, 导致 PT Cache 控制信息命中率恒为
//   ~100%(实测仅 98.27%), 无法拟合。改为按任务序号轮转散布到 SQ_PAGES /
//   CQ_PAGES 个页后, 每页首访 MISS、后续 HIT, 控制命中率由页数与包数共同决定。
// 页数与步长可用宏覆盖; CTRL_STRIDE=8 > PT_DEDUP_PREFETCH_DEPTH(<=3),
//   保证任一控制页的预取邻居不会撞到下一个控制页(否则 map_vs 重复建表)。
#ifndef TEST_CFG_NVRR_SQ_PAGES
#define TEST_CFG_NVRR_SQ_PAGES 45
#endif
#ifndef TEST_CFG_NVRR_CQ_PAGES
#define TEST_CFG_NVRR_CQ_PAGES 24
#endif
constexpr unsigned SQ_PAGES    = TEST_CFG_NVRR_SQ_PAGES;
constexpr unsigned CQ_PAGES    = TEST_CFG_NVRR_CQ_PAGES;
constexpr unsigned CTRL_STRIDE = 8;                 // 相邻控制页间隔(4KB 页为单位)
static_assert(PT_DEDUP_PREFETCH_DEPTH < CTRL_STRIDE,
              "预取深度必须小于控制页步长, 否则邻居页会撞上下一个控制页");

constexpr uint64_t SQ_IOVA = 0x22000000;            // SQ 页区基址(SQ_PAGES 页, 步长 CTRL_STRIDE)
constexpr uint64_t CQ_IOVA = 0x22400000;            // CQ 页区基址(CQ_PAGES 页, 步长 CTRL_STRIDE)
constexpr uint64_t CTRL_GPA = 0x8000000;
constexpr uint64_t CQ_GPA_OFF = 0x100000;           // CQ 的 GPA 区与 SQ 隔开 1MB
constexpr uint64_t PROBE_IOVA = 0x22800000;         // 哨兵页(独立于 SQ/CQ 区)
constexpr uint64_t HUGE = 0x200000ULL;
// 第 k 个 SQ / CQ 页的 IOVA 与 GPA(纯函数, 建表与构包共用同一套映射)
inline uint64_t sq_iova(unsigned k) { return SQ_IOVA + uint64_t(k) * CTRL_STRIDE * 4096; }
inline uint64_t sq_gpa (unsigned k) { return CTRL_GPA + uint64_t(k) * 4096; }
inline uint64_t cq_iova(unsigned k) { return CQ_IOVA + uint64_t(k) * CTRL_STRIDE * 4096; }
inline uint64_t cq_gpa (unsigned k) { return CTRL_GPA + CQ_GPA_OFF + uint64_t(k) * 4096; }

// ---- [控制包散布] 页集合按设备切分(而不是每设备都用满全部页) ----
// 为何必须切分: PT Cache / 去重 Cache / 去重 Buffer 均按 (GSCID, IOVA) 复合键隔离,
//   因此真正消耗资源的是"(设备,页) 组合数"而不是"页数"。
//   实测场景1 的 45 个 64B 页代中每个页代只属于 1 个设备 => 组合数 = 45,
//   单设备密度 1972/45 = 43.8 包每页; 24 个 16B 控制页代同理 => 84.2 包每页。
//   若改成"每设备都用满 45 页", 组合数变成 45*N=360, 去重 Buffer(320 entry)
//   的需求同步放大 N 倍 —— 实测已触发 "Buffer 满(320) + PTW 空闲(0) + 无待 flush 组"
//   的环形等待死锁(t=22.7us 后停止推进, watchdog 于 72.8us 报错)。
//   按设备切分后: 组合数 == SQ_PAGES/CQ_PAGES(与实测一致), 且单设备密度
//   250/6=41.7(SQ) 与 250/3=83.3(CQ) 也对齐实测的 43.8 / 84.2。
// 余数分给前面的设备: 45 = 8*5+5 => 设备0~4 各 6 页, 设备5~7 各 5 页。
inline unsigned ctrl_seg_base(unsigned pages, unsigned d) {
    const unsigned per = pages / N, rem = pages % N;
    return d * per + (d < rem ? d : rem);
}
inline unsigned ctrl_seg_cnt(unsigned pages, unsigned d) {
    const unsigned per = pages / N, rem = pages % N;
    return per + (d < rem ? 1u : 0u);
}
// 设备 d 的第 group 个任务所用的 SQ / CQ 页索引
// ---- [块状分配] 为何不用简单轮转(group % cnt) ----
// 实测场景1 的控制页是"页代"语义: 一批包集中访问同一页、覆盖满即关闭,
//   因此一个页的去重 Buffer entry 存活时间很短。若改用轮转, 同一控制页的包
//   会散布到整个仿真期, entry 长期挂着不释放 —— 实测已触发
//   "Buffer 满(320) + PTW 空闲(0) + 51 组待 agg_flush" 的环形等待死锁。
// 块状分配: 每页承载 blk = ceil(TASKS_DEV/cnt) 个连续任务, 既保留时间局部性,
//   又使单页密度自动对齐实测: C=1 时 SQ 250/6=41.7(实测 43.8)、CQ 250/3=83.3(实测 84.2)。
//   附带效果: 预热段(8 个伪任务, group=0..7)只落在每设备页段的第 0 页,
//   故测量段仍有 (cnt-1) 个页首访 MISS => 控制命中率 ~98.6%, 对齐实测 98.27%
//   (轮转时预热会覆盖全部页, 控制命中率退化为 ~100%, 无法拟合)。
inline unsigned sq_slot(unsigned d, uint64_t group) {
    const unsigned c = ctrl_seg_cnt(SQ_PAGES, d);
    if (!c) return 0u;
    const unsigned blk = (TASKS_DEV + c - 1) / c;      // 每页承载的连续任务数
    return ctrl_seg_base(SQ_PAGES, d) + (unsigned)((group / blk) % c);
}
inline unsigned cq_slot(unsigned d, uint64_t group) {
    const unsigned c = ctrl_seg_cnt(CQ_PAGES, d);
    if (!c) return 0u;
    const unsigned blk = (TASKS_DEV + c - 1) / c;
    return ctrl_seg_base(CQ_PAGES, d) + (unsigned)((group / blk) % c);
}
static_assert(TEST_CFG_NUM_DEVICES >= 1 && SQ_PAGES >= TEST_CFG_NUM_DEVICES &&
              CQ_PAGES >= TEST_CFG_NUM_DEVICES,
              "控制页数必须不少于设备数, 否则有设备分不到控制页");

// ------------------------------ 包型 ------------------------------
constexpr unsigned DATA_BYTES = 512, SQ_BYTES = 64, CQ_BYTES = 16;
constexpr unsigned TASK_PACKETS = 10;                    // SQ + 8x512B + CQ
constexpr unsigned TYPE_DATA = 0, TYPE_SQ = 1, TYPE_CQ = 2;
constexpr unsigned N_READ_TASKS  = TASKS_DEV / 2;        // 每设备读任务数(严格交替)
constexpr unsigned N_WRITE_TASKS = TASKS_DEV / 2;        // 每设备写任务数
// 每设备每方向包数: 读口 = 读任务(SQ+8数据) + 写任务(SQ) = TASKS_DEV/2*9 + TASKS_DEV/2*1
constexpr uint64_t RD_PKTS_PER_DEV = uint64_t(TASKS_DEV) * 5;
constexpr uint64_t WR_PKTS_PER_DEV = uint64_t(TASKS_DEV) * 5;

static_assert(N >= 1 && N <= 16, "设备数必须为1..16");
static_assert(C >= 1 && C <= 16, "每设备队列组数必须为1..16");
static_assert(TASKS > 0 && TASKS <= RANGE_PAGES, "每队列对任务数非法");
static_assert(TASKS % 2 == 0, "任务数须为偶数, 以保证读/写任务各半(严格交替)");
static_assert(QDEPTH > 0 && QDEPTH <= 1048576, "队列深度非法");
static_assert(RR_START < N, "设备轮询起点非法");
static_assert(PT_DEDUP_PREFETCH_DEPTH <= 16, "预取深度超过结果槽容量");
static_assert(uint64_t(N) * PAGES_DEV <= PAGE_PICK_RANGE,
              "全局唯一模式要求 N*PAGES_DEV 不超过可选页数(PAGE_PICK_RANGE)");
static_assert(RD_PKTS_PER_DEV + WR_PKTS_PER_DEV == uint64_t(TASKS_DEV) * TASK_PACKETS,
              "方向包数拆分与任务包数不闭合");

uint64_t spa_offset(unsigned d) { return 0x80000000ULL + uint64_t(d) * 0x10000000ULL; }

// ------------------------------ 激励数据结构 ------------------------------
// 单个包(方向/长度/地址在生成期即确定, 发送期只搬运)
struct Pkt {
    uint64_t iova = 0, expected_pa = 0, group = 0, sequence = 0;
    unsigned bytes = 0, type = TYPE_DATA;
    bool write = false;
};
// 方向突发: 一个任务在某一方向上的全部包(发送时逐包出队, 保持包序)
struct Burst {
    std::vector<Pkt> pkts;
    size_t   head = 0;
    uint64_t task = 0;             // 设备内任务号(诊断/校验用)
    bool     from_read_task = false;
};
// 一个方向的口队列(读口队列 / 写口队列)
struct DirQueue {
    std::deque<Burst> bursts;
    sc_event space_event;          // 队列腾出空间(反压释放)
    bool     done = false;          // 本 epoch 本队列的生成进程已不再向其投递
    int64_t  last_seq = -1;         // 出队包序号单调性校验
    uint64_t ready_pkts = 0;        // [A0] O(1) 维护的队列内剩余包数(含在出队的突发)
    uint64_t pushed_bursts = 0, pushed_pkts = 0, popped_pkts = 0;
    uint64_t peak_bursts = 0, full_waits = 0;
    double   full_wait_ns = 0;
};
// 设备级共享状态(上下文/页表/页集合) + 2xC 个口队列 + 仲裁游标
struct Device {
    device_context_t dc{};
    uint64_t dc_address = 0, probe_gpa = 0;
    std::vector<unsigned> pages;                  // 本设备 TASKS 个全局唯一页
    std::vector<uint64_t> gpas;                   // 共享 VS 映射: gpas[RANGE_PAGES]
    std::array<std::array<DirQueue, C>, 2> q;     // [dir][通道号]; dir 0=读口 1=写口
    uint64_t seq_ctr[2] = {0, 0};                 // 本 epoch 本方向包序号(设备内唯一)
    unsigned q_cur[2]   = {0, 0};                 // 仲裁用"设备内队列"游标(每方向独立)
    uint64_t injected[2]= {0, 0};                 // 已送达端口的包数
};

// 每方向发送侧统计(交织形态 + 供给/饱和验证)
struct DirStats {
    uint64_t sent = 0;
    uint64_t starve_cnt = 0, skip_cnt = 0, dev_repeat = 0, dq_repeat = 0;
    uint64_t dev_max_run = 0, dq_max_run = 0, q_max_run = 0;
    // [满供给阶段] 首次因空队列而跳过之前(即所有队列都有任务时)的最大连发:
    //   尾部各队列包数不完全相等(如 C=2 时 630 vs 620), 先耗尽的队列会迫使
    //   剩余包全来自另一队列 => 全程 q_max_run 会被尾部拉高, 故分开统计。
    uint64_t first_skip_sent = UINT64_MAX;
    uint64_t dev_max_run_pre = 0, dq_max_run_pre = 0, q_max_run_pre = 0;
    uint64_t dev_run = 0, dq_run = 0, q_run = 0;
    int      last_dev = -1, last_dq = -1;
    unsigned last_q = 0;
    double   starve_ns = 0, first_send_ns = -1, last_send_ns = 0;
    bool     inject_done = false;
    // [A0 饱和门禁] 供给侧"从未成为瓶颈"的直接证据(每 epoch 重置)
    uint64_t empty_sel_cnt = 0;             // RR 首选队列为空且其生成进程未结束的次数
    uint64_t first_done_sent = UINT64_MAX;  // 首次选到"生成已结束(done)"队列时的已发包数
    uint64_t sel_live_cnt = 0;              // 选到"生成未结束"队列的次数(采样数)
    uint64_t min_occ_live = UINT64_MAX;     // 上述采样中选中队列出队前包占用的最小值
    uint64_t occ_sum = 0, occ_cnt = 0;      // 选中队列出队前占用的均值采样
    double   gen_block_ns = 0;              // 本方向生成进程累计阻塞时间(反压)
    bool     prefill_ok = false;            // 预填充屏障是否通过
    // 同一 4KB 页(group)相邻数据包在本方向端口流上的间隔(单位: 包)
    std::map<uint64_t, uint64_t> group_last_idx;
    uint64_t gap_sum = 0, gap_cnt = 0, gap_max = 0;
    std::string order_sample;
};

// 所有 SystemC 进程属于单个 RP; 每个端口 socket 只被对应方向的发送进程调用。
class NvmeRrArbScenario {
    RP_Module& rp;
    iommu_top& top;
    std::array<Device, N> devices;
    std::array<DirStats, 2> ds;
    std::vector<std::unique_ptr<tlm_generic_payload>> payloads;
    uint64_t total_responded = 0;
    sc_event phase_event;                 // epoch 切换
    sc_event dir_ready[2];                // 某方向有新包可发(跨设备)
    unsigned epoch = 0;
    bool stopped = false;
    double start_ns = 0;
    uint64_t last_progress = 0;
    double last_progress_ns = 0;

    // ------------------------------------------------------------------
    // 页表构建辅助(与旧场景2/3 nvme10 路径逐句同构)
    // ------------------------------------------------------------------
    static std::vector<unsigned> choose(unsigned count, unsigned range, unsigned seed, unsigned shuffle_seed) {
        std::mt19937 rng(seed), permutation(shuffle_seed);
        std::set<unsigned> chosen;
        while (chosen.size() < count) chosen.insert(rng() % range);
        std::vector<unsigned> result(chosen.begin(), chosen.end());
        for (size_t i = result.size(); i > 1; --i) std::swap(result[i-1], result[permutation() % i]);
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
        require(rp.add_g_stage_pte(&top,dev.dc.iohgatp,gpa,p,level)!=uint64_t(-1),"构建G-stage失败");
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

    void setup() {
        // 保留现有低地址寄存器/CQ区域，只初始化一次物理分配游标。
        next_free_page=std::max<uint64_t>(next_free_page,240);
        // 最坏页表预算：每个2MiB IOVA区一张L0表，另留目录、G-stage及裕量。
        const uint64_t per_device_pages=(uint64_t(RANGE_MB)+2)/2+64;
        require((next_free_page+1+N*per_device_pages)*4096 <= DDR_Module::DDR_MEMORY_SIZE,
                "所有设备页表预算超过32MiB DDR");
        require(rp.enable_iommu(&top,DDT_1LVL)>=0,"启用IOMMU失败");

        // [全局唯一独立页] 一次性抽取 N*PAGES_DEV 个互不重复的 4KB 页, 每设备切 PAGES_DEV 个,
        //   每任务独占 1 页(8x512B 覆盖整页) => 跨设备/跨任务/跨队列页集合两两不相交。
        std::vector<unsigned> all_pages = choose(N*PAGES_DEV, PAGE_PICK_RANGE, 2024, 123);

        for(unsigned d=0;d<N;++d) {
            auto& dev=devices[d];
            dev.pages.assign(all_pages.begin()+uint64_t(d)*PAGES_DEV,
                             all_pages.begin()+uint64_t(d+1)*PAGES_DEV);
            dev.probe_gpa=0x100000ULL+uint64_t(d)*4096;
            next_free_gpage[d+1]=0x10000;
            // [建表统一 20261103] 与 real8 同构: MSIPTP_Off(本场景不合成 MSI)。
            dev.dc_address=rp.add_device(&top,d,d+1,0,0,0,0,0,1,1,0,0,0,
                                        IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Off,0,0,0);
            require(dev.dc_address!=uint64_t(-1),"添加设备失败");
            rp.read_memory_test_rp(dev.dc_address,sizeof(dev.dc),reinterpret_cast<char*>(&dev.dc));
            dev.dc.ta.PSCID=0;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&dev.dc),dev.dc_address,sizeof(dev.dc));
            device_context_t readback{};
            rp.read_memory_test_rp(dev.dc_address,sizeof(readback),reinterpret_cast<char*>(&readback));
            require(readback.iohgatp.GSCID==d+1 && readback.ta.PSCID==0 && readback.tc.PDTV==0,
                    "DC读回上下文不匹配");

            dev.gpas.resize(RANGE_PAGES);
            std::mt19937 mapping(42+d);
            // 先生成全范围映射值(每设备种子不同 -> 各设备 S1/S2 结果不同)。
            for(unsigned i=0;i<RANGE_PAGES;++i) {
                unsigned h=mapping()%HUGE_COUNT, slot=mapping()%512;
                dev.gpas[i]=uint64_t(h)*HUGE+uint64_t(slot)*4096;
            }
            // [建表统一 20261103] 与 real8 同序: gpas -> GS 50x2MiB 大页叶 -> VS。
            for(unsigned h=0;h<HUGE_COUNT;++h) map_g(dev,uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(d),1);
            // VS 数据映射: 全范围预建(与 real8 同构, 覆盖全部预取邻居)。
            for(unsigned i=0;i<RANGE_PAGES;++i) map_vs(dev,IOVA_BASE+uint64_t(i)*4096,dev.gpas[i]);
            // [控制包散布] SQ_PAGES 个 SQ 页 + CQ_PAGES 个 CQ 页, 逐页做 VS+G 两级建表,
            //   并为每个控制页建 PT_DEDUP_PREFETCH_DEPTH 个后继邻居(预取路径需要邻居已建表)。
            //   邻居 GPA 落在 0x200000/0x400000/0x600000 起, 均已被上面 50x2MiB 大页覆盖,
            //   故不需单独 map_g; 而 SQ/CQ 本体 GPA(>=128MB) 在大页区外, 必须逐页 map_g。
            for(unsigned k=0;k<SQ_PAGES;++k) {
                map_vs(dev,sq_iova(k),sq_gpa(k));
                map_g (dev,sq_gpa(k),sq_gpa(k)+spa_offset(d),0);
                for(unsigned j=1;j<=PT_DEDUP_PREFETCH_DEPTH;++j)
                    map_vs(dev,sq_iova(k)+uint64_t(j)*4096,
                           0x200000ULL+uint64_t(k*CTRL_STRIDE+j)*4096);
            }
            for(unsigned k=0;k<CQ_PAGES;++k) {
                map_vs(dev,cq_iova(k),cq_gpa(k));
                map_g (dev,cq_gpa(k),cq_gpa(k)+spa_offset(d),0);
                for(unsigned j=1;j<=PT_DEDUP_PREFETCH_DEPTH;++j)
                    map_vs(dev,cq_iova(k)+uint64_t(j)*4096,
                           0x400000ULL+uint64_t(k*CTRL_STRIDE+j)*4096);
            }
            // 独立哨兵使各设备S1结果也不同；不计入正式性能请求。
            map_vs(dev,PROBE_IOVA,dev.probe_gpa);
            for(unsigned k=1;k<=PT_DEDUP_PREFETCH_DEPTH;++k)
                map_vs(dev,PROBE_IOVA+uint64_t(k)*4096,0x600000ULL+uint64_t(k)*4096);
            uint64_t digest=1469598103934665603ULL;
            for(unsigned page:dev.pages) digest=(digest^page)*1099511628211ULL;
            std::cout << "[MDMC_SETUP] DID=" << d << " GSCID=" << d+1 << " PSCID=0 chan=" << C
                      << " pages=" << RANGE_PAGES << " groups=" << PAGES_DEV
                      << " load_mode=" << (PER_QUEUE?"per_queue":"fixed_total")
                      << " sequence_hash=" << digest << " root=" << dev.dc.iohgatp.PPN
                      << " free_ppn=" << next_free_page << '\n';
            top.md_trace.progress();
        }
        // [全局唯一校验] 汇总所有设备的页号, 断言两两不重复且总数 = N*PAGES_DEV。
        {
            std::set<unsigned> uniq; unsigned total=0;
            for(unsigned d=0;d<N;++d) {
                uniq.insert(devices[d].pages.begin(),devices[d].pages.end());
                total+=devices[d].pages.size();
            }
            require(uniq.size()==total && total==N*PAGES_DEV,
                    "全局唯一页校验失败: 存在跨设备/跨任务重复页");
            std::cout << "[MDMC_UNIQUE_OK] devices=" << N << " chan/dev=" << C
                      << " load_mode=" << (PER_QUEUE?"per_queue":"fixed_total")
                      << " pages/dev=" << PAGES_DEV << " tasks/dev=" << TASKS_DEV
                      << " total_unique=" << uniq.size() << " expected=" << N*PAGES_DEV << '\n';
        }
        // [建表统一 20261103] 建表完成后统一全局失效一次 + 页表指纹。
        invalidate_all();
        std::cout << "[MDMC_TB_FINGERPRINT] msiptp=Off huge_leaves=" << (unsigned)HUGE_COUNT * N
                  << " vs_policy=full_range order=gpas->GS_huge->VS->ctrl/probe->inval" << '\n';
    }

    // ------------------------------------------------------------------
    // 包构造(纯函数, 不推进仿真时间; 供生成进程与包型自检共用)
    // ------------------------------------------------------------------
    Pkt mk_sq(unsigned d, uint64_t group) const {
        // [控制包散布] 在本设备的 SQ 页段内轮转, 与建表时的 sq_iova/sq_gpa 同式
        const unsigned k=sq_slot(d,group);
        Pkt p;
        p.type=TYPE_SQ; p.bytes=SQ_BYTES; p.write=false;
        p.iova=sq_iova(k); p.expected_pa=sq_gpa(k)+spa_offset(d);
        p.group=group;
        return p;
    }
    Pkt mk_cq(unsigned d, uint64_t group) const {
        const unsigned k=cq_slot(d,group);
        Pkt p;
        p.type=TYPE_CQ; p.bytes=CQ_BYTES; p.write=true;
        p.iova=cq_iova(k); p.expected_pa=cq_gpa(k)+spa_offset(d);
        p.group=group;
        return p;
    }
    Pkt mk_data(unsigned d, unsigned page_slot, unsigned k, uint64_t group, bool is_read) const {
        const unsigned page=devices[d].pages[page_slot];
        Pkt p;
        p.type=TYPE_DATA; p.bytes=DATA_BYTES; p.write=is_read ? false : true;
        p.iova=IOVA_BASE+uint64_t(page)*4096+uint64_t(k)*DATA_BYTES;
        p.expected_pa=devices[d].gpas[page]+spa_offset(d)+uint64_t(k)*DATA_BYTES;
        p.group=group;
        return p;
    }
    Pkt mk_probe(unsigned d) const {
        Pkt p;
        p.type=TYPE_DATA; p.bytes=DATA_BYTES; p.write=false;
        p.iova=PROBE_IOVA; p.expected_pa=devices[d].probe_gpa+spa_offset(d);
        p.group=0;
        return p;
    }
    // 把一个 NVMe 任务拆成读向/写向两个方向突发(包序: SQ -> 8x数据 -> CQ)。
    void build_task(unsigned d, unsigned page_slot, bool is_read, uint64_t group,
                    Burst& br, Burst& bw) const {
        br.task=group; bw.task=group; br.from_read_task=is_read; bw.from_read_task=is_read;
        br.pkts.push_back(mk_sq(d,group));                              // SQ: 读口
        for(unsigned k=0;k<8;++k) {                                     // 8x512B: 任务方向
            Pkt p=mk_data(d,page_slot,k,group,is_read);
            (is_read ? br : bw).pkts.push_back(p);
        }
        bw.pkts.push_back(mk_cq(d,group));                              // CQ: 写口
    }

    // ------------------------------------------------------------------
    // 派发路由(纯函数): 任务生成序号 slot -> (读口队列 rq, 写口队列 wq, 页槽, group)
    //   规则(用户 20261105 定义, 同索引派发):
    //     任务 #i: [SQ+8数据] -> rQ[i%C], [CQ] / [8数据+CQ] -> wQ[i%C]   (同索引)
    //   => 每个读/写队列内部的数据页集合互不相交且均为新页, 且每队列读写混合。
    //   per_queue: 生成序 = 对每个 m 轮流遍历通道 c, 每个通道各自一条独立任务流
    //              (流内严格读写交替), 页槽 = c*TASKS + k; 每队列突数/包数精确相等;
    //   fixed_total: 生成序 = 全局任务号 t(偶读奇写), 页槽 = t, 通道 = (t/2)%C。
    // ------------------------------------------------------------------
    struct Route {
        unsigned rq = 0, wq = 0, page_slot = 0;
        uint64_t group = 0;
        bool is_read = false;
    };
    static Route route_of(uint64_t slot) {
        Route r;
        if (PER_QUEUE) {
            const unsigned half = unsigned(slot % 2);        // 0=读任务 1=写任务
            const uint64_t pair = slot / 2;                  // = m*C + c
            const unsigned c = unsigned(pair % C);
            const uint64_t m = pair / C;
            const uint64_t k = 2*m + half;                   // 流内任务号
            r.is_read   = (half == 0u);
            r.page_slot = unsigned(c*TASKS + k);
            r.group     = r.page_slot;
            r.rq = r.wq = c;
        } else {
            const uint64_t n = slot / 2;                     // 同类型任务序号
            r.is_read   = (slot % 2 == 0);
            r.page_slot = unsigned(slot);
            r.group     = slot;
            r.rq = r.wq = unsigned(n % C);
        }
        return r;
    }

    // 包型自检: 只校验描述符与派发路由, 不访问端口、不推进仿真时间。
    void validate_pattern() {
        uint64_t data=0, sq=0, cq=0, msi=0, bytes=0, rd_pkts=0, wr_pkts=0;
        // [控制包散布] 统计 SQ/CQ 实际命中的不同 4KB 页, 用于校验页数
        std::set<uint64_t> sq_pages_seen, cq_pages_seen;
        for(unsigned d=0;d<N;++d) {
            std::set<unsigned> rd_q_pages[C], wr_q_pages[C];
            for(uint64_t slot=0;slot<TASKS_DEV;++slot) {
                const Route rt=route_of(slot);
                require(rt.page_slot<PAGES_DEV,"页槽越界");
                // [同索引派发] 一个任务的两个方向突发必须进入同一索引的读/写口队列
                require(rt.rq==rt.wq,"派发路由不是同索引(读口队列 != 写口队列)");
                Burst br,bw;
                build_task(d, rt.page_slot, rt.is_read, rt.group, br, bw);
                require(br.pkts.size()==(rt.is_read?9u:1u),"读向突发包数错误");
                require(bw.pkts.size()==(rt.is_read?1u:9u),"写向突发包数错误");
                // 数据页归属: 读任务的 8 包在 rQ[rq], 写任务的 8 包在 wQ[wq]
                (rt.is_read ? rd_q_pages[rt.rq] : wr_q_pages[rt.wq]).insert(rt.page_slot);
                // 还原任务内包序: SQ -> 8x数据 -> CQ
                std::vector<Pkt> ordered;
                ordered.push_back(br.pkts[0]);
                if(rt.is_read) for(size_t k=1;k<br.pkts.size();++k) ordered.push_back(br.pkts[k]);
                else           for(size_t k=0;k+1<bw.pkts.size();++k) ordered.push_back(bw.pkts[k]);
                ordered.push_back(bw.pkts.back());
                require(ordered.size()==TASK_PACKETS,"任务包数不是10");
                uint64_t page_base=0;
                for(unsigned k=0;k<TASK_PACKETS;++k) {
                    const Pkt& p=ordered[k];
                    require(p.group==rt.group,"任务内 group 不一致");
                    if(k==0) {
                        // [控制包散布] SQ 不再固定单页: 改为校验"落在 SQ 页区内 + 步长对齐
                        //   + 页索引与任务序号轮转一致", 强度不低于原来的等值断言。
                        require(p.type==TYPE_SQ && !p.write && p.bytes==SQ_BYTES,
                                "首包必须是 SQ 读包(64B)");
                        require(p.iova>=SQ_IOVA && p.iova<sq_iova(SQ_PAGES) &&
                                ((p.iova-SQ_IOVA)%(CTRL_STRIDE*4096))==0,
                                "SQ 包未落在 SQ 页区的合法页边界上");
                        require(p.iova==sq_iova(sq_slot(d,rt.group)) &&
                                p.expected_pa==sq_gpa(sq_slot(d,rt.group))+spa_offset(d),
                                "SQ 页索引与设备页段轮转规则不一致");
                        sq_pages_seen.insert(p.iova);
                        ++sq;
                    } else if(k==TASK_PACKETS-1) {
                        require(p.type==TYPE_CQ && p.write && p.bytes==CQ_BYTES,
                                "末包必须是 CQ 写包(16B)");
                        require(p.iova>=CQ_IOVA && p.iova<cq_iova(CQ_PAGES) &&
                                ((p.iova-CQ_IOVA)%(CTRL_STRIDE*4096))==0,
                                "CQ 包未落在 CQ 页区的合法页边界上");
                        require(p.iova==cq_iova(cq_slot(d,rt.group)) &&
                                p.expected_pa==cq_gpa(cq_slot(d,rt.group))+spa_offset(d),
                                "CQ 页索引与设备页段轮转规则不一致");
                        cq_pages_seen.insert(p.iova);
                        ++cq;
                    } else {
                        require(p.type==TYPE_DATA && p.bytes==DATA_BYTES,"数据包必须是512B");
                        require(p.write==!rt.is_read,"数据包方向与任务类型不一致");
                        if(k==1) { page_base=p.iova; require((page_base&4095)==0,"数据组页基址未对齐"); }
                        require(p.iova==page_base+uint64_t(k-1)*DATA_BYTES,
                                "数据组不是同一4KB页内的连续8个512B");
                        ++data;
                    }
                    require(p.type!=3,"本场景禁止发送MSI");
                    bytes+=p.bytes;
                }
                rd_pkts+=br.pkts.size(); wr_pkts+=bw.pkts.size();
            }
            // [每队列全为新页] 同方向各队列数据页集合两两不相交; 读口与写口也互不相交;
            //   并集恰等于本设备页槽数 => 每个通道内部都是不同的新 4KB 页。
            std::set<unsigned> union_all;
            for(unsigned c=0;c<C;++c) {
                for(unsigned c2=c+1;c2<C;++c2) {
                    std::set<unsigned> i1, i2;
                    std::set_intersection(rd_q_pages[c].begin(), rd_q_pages[c].end(),
                                          rd_q_pages[c2].begin(), rd_q_pages[c2].end(),
                                          std::inserter(i1, i1.begin()));
                    std::set_intersection(wr_q_pages[c].begin(), wr_q_pages[c].end(),
                                          wr_q_pages[c2].begin(), wr_q_pages[c2].end(),
                                          std::inserter(i2, i2.begin()));
                    require(i1.empty() && i2.empty(),
                            "同方向不同队列的数据页集合相交(通道内不是全新高页)");
                }
                std::set<unsigned> ix;
                std::set_intersection(rd_q_pages[c].begin(), rd_q_pages[c].end(),
                                      wr_q_pages[c].begin(), wr_q_pages[c].end(),
                                      std::inserter(ix, ix.begin()));
                require(ix.empty(), "读口队列与写口队列的数据页集合相交");
                union_all.insert(rd_q_pages[c].begin(), rd_q_pages[c].end());
                union_all.insert(wr_q_pages[c].begin(), wr_q_pages[c].end());
            }
            require(union_all.size()==PAGES_DEV, "各队列数据页并集不等于每设备页数");
        }
        require(msi==0,"出现MSI包");
        require(data==uint64_t(N)*TASKS_DEV*8 && sq==uint64_t(N)*TASKS_DEV && cq==sq,
                "包型数量不闭合");
        require(bytes==uint64_t(N)*TASKS_DEV*4176,"字节总数不闭合(期望每任务4176B)");
        require(rd_pkts==uint64_t(N)*RD_PKTS_PER_DEV && wr_pkts==uint64_t(N)*WR_PKTS_PER_DEV,
                "读口/写口包数与预期不一致");
        require(N_READ_TASKS==N_WRITE_TASKS,"读/写任务数不相等(严格交替要求任务数为偶数)");
        // [控制包散布] 页数校验: 任务数不少于页数时必须正好用满 SQ_PAGES / CQ_PAGES 个页。
        //   这是"控制信息 PT Cache 命中率不再是 100%"的前提: 每页首访 MISS、后续 HIT。
        const uint64_t sq_expect=std::min<uint64_t>(SQ_PAGES,TASKS_DEV);
        const uint64_t cq_expect=std::min<uint64_t>(CQ_PAGES,TASKS_DEV);
        require(sq_pages_seen.size()==sq_expect,"SQ 实际使用的页数与 SQ_PAGES 不符");
        require(cq_pages_seen.size()==cq_expect,"CQ 实际使用的页数与 CQ_PAGES 不符");
        std::cout << "[NVRR_PATTERN_OK] load_mode=" << (PER_QUEUE?"per_queue":"fixed_total")
                  << " gen_proc/dev=" << C << " bound=queue_pair"
                  << " tasks/dev=" << TASKS_DEV << " (read=" << N_READ_TASKS
                  << " write=" << N_WRITE_TASKS << ") pkts/queue_pair=" << TASKS
                  << " data512=" << data << " sq64=" << sq << " cq16=" << cq
                  << " msi=0 bytes=" << bytes << " avg_bytes=" << (bytes/(double)(data+sq+cq))
                  << " order=SQ,DATA8,CQ msiptp=Off sq_route=same_index"
                  << " pages_disjoint_per_queue=OK"
                  << " rd_port_pkts=" << rd_pkts << " wr_port_pkts=" << wr_pkts << '\n';
        // [控制包散布] 单独一行输出控制页几何与密度, 供与实测场景1 对账。
        //   按设备切分后每个控制页只被 1 个设备使用, 故 (设备,页) 组合数 == 页数,
        //   sq_pkts_per_page 就是"单组合密度", 可直接与实测 43.8 / 84.2 对比。
        //   expect_ctrl_pt_hit_pct 是"假设每页首访 MISS、无预热"的理论下限。
        std::cout << "[NVRR_CTRL_SPREAD] sq_pages=" << sq_pages_seen.size()
                  << " cq_pages=" << cq_pages_seen.size()
                  << " dev_page_pairs=" << (sq_pages_seen.size() + cq_pages_seen.size())
                  << " stride=" << CTRL_STRIDE << " scope=per_device_split"
                  << " sq_pkts_per_page=" << (sq_pages_seen.size()? (double)sq/(double)sq_pages_seen.size() : 0.0)
                  << " cq_pkts_per_page=" << (cq_pages_seen.size()? (double)cq/(double)cq_pages_seen.size() : 0.0)
                  << " expect_ctrl_pt_hit_pct="
                  << (sq+cq ? 100.0*(double)(sq+cq-sq_pages_seen.size()-cq_pages_seen.size())/(double)(sq+cq) : 0.0)
                  << " (实测参照: SQ 43.8/页, CQ 84.2/页, 控制命中率 98.27%)\n";
    }

    // 每 epoch 期望包数(哨兵/预热/测量)
    static uint64_t expected_count(unsigned e) {
        if(e==1) return uint64_t(N)*SENTINEL_PKTS;
        if(e==2) return uint64_t(N)*WARMUP_TASKS*2;
        return uint64_t(N)*TASKS_DEV*TASK_PACKETS;
    }
    static uint64_t expected_dir_count(unsigned e, unsigned dir) {
        if(e==1) return dir==0 ? uint64_t(N)*SENTINEL_PKTS : 0ull;
        if(e==2) return uint64_t(N)*WARMUP_TASKS;
        return uint64_t(N)*(dir==0 ? RD_PKTS_PER_DEV : WR_PKTS_PER_DEV);
    }

    // ------------------------------------------------------------------
    // 入队(带反压): 队列满则本通道生成进程等待; 无界默认模式下永不等待。
    //   只被自己绑定的队列反压 => 通道间零耦合。
    // ------------------------------------------------------------------
    void push_burst(unsigned d, unsigned dir, unsigned c, Burst b) {
        auto& q=devices[d].q[dir][c];
        if(q.bursts.size()>=QDEPTH) {
            const double t0=iommu_md::now_ns(); ++q.full_waits;
            while(q.bursts.size()>=QDEPTH && !stopped) wait(q.space_event);
            const double dt=iommu_md::now_ns()-t0;
            q.full_wait_ns+=dt; ds[dir].gen_block_ns+=dt;
        }
        if(stopped) return;
        // 方向内包序号统一编号(设备内该方向单调递增; 每队列内因单生产者必然递增)
        for(auto& p:b.pkts) p.sequence=devices[d].seq_ctr[dir]++;
        const uint64_t np=b.pkts.size();
        q.bursts.push_back(std::move(b));
        q.pushed_bursts++; q.pushed_pkts+=np; q.ready_pkts+=np;
        if(q.bursts.size()>q.peak_bursts) q.peak_bursts=q.bursts.size();
        dir_ready[dir].notify(SC_ZERO_TIME);
    }

    // ------------------------------------------------------------------
    // [A0-3] 预填充屏障(冷启动饱和): 本方向所有队列都达到 min(QDEPTH,8) 个突发
    //   或已 done 才允许进入仲裁循环 => 第一次仲裁必命中就绪任务。
    //   无界模式(QDEPTH=TASKS+8)下屏障在零时间内即满足, 成本为 no-op;
    //   有界模式下消除冷启动空访问(但不能消除稳态期的经验性风险)。
    // ------------------------------------------------------------------
    void prefill_barrier(unsigned dir) {
        const unsigned need=std::min<unsigned>(QDEPTH,8u);
        while(!stopped) {
            bool ok=true;
            for(unsigned d=0;d<N && ok;++d)
                for(unsigned c=0;c<C;++c) {
                    const auto& q=devices[d].q[dir][c];
                    if(q.bursts.size()<need && !q.done) { ok=false; break; }
                }
            if(ok) break;
            wait(dir_ready[dir]);
        }
        ds[dir].prefill_ok=!stopped;
    }

    // ------------------------------------------------------------------
    // 生成进程: 每设备 C 个, 每个独占绑定 (读口队列 c, 写口队列 c)。
    //   - c==0 额外承担 epoch1 哨兵与 epoch2 预热的发出(保持设备级共享
    //     DC/SQ/CQ 路径与旧批次可比); c>0 在 epoch1/2 不产出但同样标记 done;
    //   - 每个进程内部 k=0..TASKS-1 严格读写交替(偶=读任务), 同索引入队;
    //   - 只被自己绑定的两个队列反压 => 通道间零耦合。
    // ------------------------------------------------------------------
    void generator(unsigned d, unsigned c) {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            const unsigned e=epoch;
            if(e==1) {
                if(c==0) {
                    // 哨兵: 2 个 PROBE 读包 -> 读口队列0(设备级共享翻译路径打通)
                    Burst b; b.task=0; b.from_read_task=true;
                    for(unsigned k=0;k<SENTINEL_PKTS;++k) b.pkts.push_back(mk_probe(d));
                    push_burst(d,0,0,std::move(b));
                }
            } else if(e==2) {
                if(c==0) {
                    // 预热: 8 个伪任务, 每个 = SQ(读) + CQ(写), 不含 MSI/数据
                    for(unsigned t=0;t<WARMUP_TASKS;++t) {
                        Burst br,bw; br.task=t; bw.task=t;
                        br.pkts.push_back(mk_sq(d,t));
                        bw.pkts.push_back(mk_cq(d,t));
                        push_burst(d,0,0,std::move(br));
                        push_burst(d,1,0,std::move(bw));
                    }
                }
            } else if(PER_QUEUE) {
                // per_queue: 本通道自带 TASKS 个任务 / TASKS 个独立页, 流内严格读写交替
                for(unsigned k=0;k<TASKS;++k) {
                    const bool     is_read=(k%2==0);
                    const unsigned page_slot=c*TASKS+k;
                    const uint64_t group=page_slot;
                    Burst br,bw;
                    build_task(d,page_slot,is_read,group,br,bw);
                    push_burst(d,0,c,std::move(br));
                    push_burst(d,1,c,std::move(bw));
                }
            } else {
                // fixed_total(旧口径): 每设备恒 TASKS 任务, 按任务号同索引切给 C 个队列对
                for(uint64_t t=0;t<TASKS;++t) {
                    const Route rt=route_of(t);
                    if(rt.rq!=c) continue;           // 本通道只产出路由到自己队列对的任务
                    Burst br,bw;
                    build_task(d, rt.page_slot, rt.is_read, rt.group, br, bw);
                    push_burst(d,0,c,std::move(br));
                    push_burst(d,1,c,std::move(bw));
                }
            }
            // 本 epoch 本通道生成结束: 只标记自己绑定的两个队列 done
            for(unsigned dir=0;dir<2;++dir) {
                devices[d].q[dir][c].done=true;
                dir_ready[dir].notify(SC_ZERO_TIME);
            }
            previous=e;
            top.md_trace.progress();
        }
    }

    // ------------------------------------------------------------------
    // 发送进程: 每方向 1 个, 独占本方向端口(设备间仲裁需求1~5)
    //   嵌套 RR: 外层设备、内层设备内队列; 每步只出队 1 个包后切换设备。
    //   进程常驻: 外层按 epoch 等待 phase_event, 与生成进程同节奏(每 epoch 不重复 spawn)。
    // ------------------------------------------------------------------
    void sender(bool is_write) {
        const unsigned dir=is_write?1u:0u;
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            prefill_barrier(dir);
            if(stopped) return;
            inject_epoch(dir);
            if(stopped) return;
            // 本方向本 epoch 注入结束; 两方向都结束后关闭 IOMMU 侧"注入并发窗口"。
            ds[dir].inject_done=true;
            if(ds[0].inject_done && ds[1].inject_done) {
                top.conc_window_close();
                if(epoch==iommu_md::MEASUREMENT_EPOCH)
                    std::cout << "[MDMC_INJECT_END] epoch=" << epoch
                              << " t=" << iommu_md::now_ns()
                              << " last_send_r=" << ds[0].last_send_ns
                              << " last_send_w=" << ds[1].last_send_ns << '\n';
            }
            previous=epoch;
            top.md_trace.progress();
        }
    }

    // 本 epoch 单方向注入: 嵌套 RR 选包 -> 独占本方向端口发送, 直到本方向队列全部排空。
    void inject_epoch(unsigned dir) {
        const bool is_write=(dir==1u);
        auto& st=ds[dir];
        unsigned dev_cur=RR_START%N;
        while(!stopped) {
            // ---- [A0] RR 首选(设备,队列)是否已有就绪任务(尾部排空阶段除外) ----
            {
                const unsigned d0=dev_cur;
                const auto& q0=devices[d0].q[dir][devices[d0].q_cur[dir]];
                if(q0.bursts.empty() && !q0.done) ++st.empty_sel_cnt;
            }
            // ---- 嵌套轮询选出 (设备, 队列) ----
            int sel_d=-1; unsigned sel_c=0;
            for(unsigned k=0;k<N && sel_d<0;++k) {
                const unsigned d=(dev_cur+k)%N;
                auto& dev=devices[d];
                for(unsigned j=0;j<C;++j) {
                    const unsigned c=(dev.q_cur[dir]+j)%C;
                    if(!dev.q[dir][c].bursts.empty()) {
                        sel_d=int(d); sel_c=c;
                        dev.q_cur[dir]=(c+1)%C;      // 设备内队列游标每步 +1
                        dev_cur=(d+1)%N;             // 设备游标切到下一个设备
                        if(j) {                      // 跳过了 j 个空队列(只应发生在注入尾部)
                            st.skip_cnt+=j;
                            if(st.first_skip_sent==UINT64_MAX) st.first_skip_sent=st.sent;
                        }
                        break;
                    }
                }
            }
            if(sel_d<0) {
                // 无就绪任务: 全部队列已 done 且空 => 本方向注入结束
                bool all_done=true;
                for(unsigned d=0;d<N && all_done;++d)
                    for(unsigned c=0;c<C;++c) {
                        auto& q=devices[d].q[dir][c];
                        if(!q.done || !q.bursts.empty()) { all_done=false; break; }
                    }
                if(all_done) break;
                const double t0=iommu_md::now_ns(); ++st.starve_cnt;
                wait(dir_ready[dir]);
                st.starve_ns+=iommu_md::now_ns()-t0;
                continue;
            }
            const unsigned d=unsigned(sel_d);
            auto& dev=devices[d];
            auto& q=dev.q[dir][sel_c];

            // ---- [A0 饱和采样] 选中队列出队前的包占用(尾部排空除外) ----
            if(!q.done) {
                ++st.sel_live_cnt;
                if(q.ready_pkts<st.min_occ_live) st.min_occ_live=q.ready_pkts;
            } else if(st.first_done_sent==UINT64_MAX) {
                st.first_done_sent=st.sent;
            }
            st.occ_sum+=q.ready_pkts; ++st.occ_cnt;

            // ---- 出队 1 个包(突发耗尽则弹出并释放反压) ----
            Burst& b=q.bursts.front();
            Pkt p=b.pkts[b.head++];
            if(b.head==b.pkts.size()) {
                q.bursts.pop_front();
                q.space_event.notify(SC_ZERO_TIME);
            }
            --q.ready_pkts;
            q.popped_pkts++; dev.injected[dir]++;
            require(p.write==is_write,"方向队列出现异向包(读口只允许读包/写口只允许写包)");
            require(int64_t(p.sequence)>q.last_seq,"队列内包序号非单调递增");
            q.last_seq=int64_t(p.sequence);

            // ---- 交织形态统计(需求5 的量化证据) ----
            const int dq=int(d*C+sel_c);
            if(st.sent) {
                if(int(d)==st.last_dev) { ++st.dev_repeat; if(++st.dev_run>st.dev_max_run) st.dev_max_run=st.dev_run; }
                else st.dev_run=1;
                if(dq==st.last_dq) { ++st.dq_repeat; if(++st.dq_run>st.dq_max_run) st.dq_max_run=st.dq_run; }
                else st.dq_run=1;
                if(sel_c==st.last_q) { if(++st.q_run>st.q_max_run) st.q_max_run=st.q_run; }
                else st.q_run=1;
            } else { st.dev_run=st.dq_run=st.q_run=1; st.dev_max_run=st.dq_max_run=st.q_max_run=1; }
            // 满供给阶段(尚未出现空队列跳过)的连发上界
            if(st.first_skip_sent==UINT64_MAX) {
                if(st.dev_run>st.dev_max_run_pre) st.dev_max_run_pre=st.dev_run;
                if(st.dq_run >st.dq_max_run_pre)  st.dq_max_run_pre =st.dq_run;
                if(st.q_run  >st.q_max_run_pre)   st.q_max_run_pre  =st.q_run;
            }
            st.last_dev=int(d); st.last_dq=dq; st.last_q=sel_c;
            if(epoch==iommu_md::MEASUREMENT_EPOCH) {
                if(p.type==TYPE_DATA) {
                    const uint64_t key=(uint64_t(d)<<32)|p.group;
                    auto it=st.group_last_idx.find(key);
                    if(it!=st.group_last_idx.end()) {
                        const uint64_t gap=st.sent-it->second;
                        st.gap_sum+=gap; st.gap_cnt++;
                        if(gap>st.gap_max) st.gap_max=gap;
                    }
                    st.group_last_idx[key]=st.sent;
                }
                if(st.sent<32) {
                    char buf[24];
                    snprintf(buf,sizeof(buf),"d%uq%u:t%u ",d,sel_c,p.type);
                    st.order_sample+=buf;
                }
            }

            // ---- 构造 payload 并独占调用本方向端口 ----
            iommu_md::Request record;
            record.device=d; record.epoch=epoch; record.type=p.type; record.bytes=p.bytes;
            record.sequence=p.sequence; record.group=p.group;
            record.iova=p.iova; record.expected_pa=p.expected_pa; record.write=p.write;
            record.ready_ns=iommu_md::now_ns();
            auto trans=std::make_unique<tlm_generic_payload>();
            trans->set_address(p.iova);
            trans->set_data_ptr(new unsigned char[p.bytes]());
            trans->set_data_length(p.bytes);
            trans->set_streaming_width(p.bytes);
            trans->set_command(p.write ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
            trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
            auto* ext=new PayloadExtention();
            ext->requester_id=d; ext->pid_valid=0; ext->process_id=0;
            ext->exec_req=0; ext->priv_req=0; ext->no_write=p.write?0:1;
            ext->at=0; ext->is_ctrl=(p.type!=TYPE_DATA);
            trans->set_extension(ext);
            auto* ptr=trans.get();
            payloads.push_back(std::move(trans));
            // grant_ns = 端口发送点(权威注入时刻)
            record.grant_ns=iommu_md::now_ns();
            top.md_trace.register_request(ptr,record);
            if(st.first_send_ns<0) st.first_send_ns=record.grant_ns;

            sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
            auto status = is_write
                ? rp.axi_master_to_pcie_noc_0w_socket->nb_transport_fw(*ptr,phase,delay)
                : rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr,phase,delay);
            require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
            st.sent++;
            st.last_send_ns=iommu_md::now_ns();
            if((st.sent & 0xFFF)==0) top.md_trace.progress();
        }
    }

    void watchdog() {
        while(!stopped) {
            wait(1000,SC_NS);
            if(top.md_trace.progress_serial!=last_progress) {
                last_progress=top.md_trace.progress_serial; last_progress_ns=iommu_md::now_ns();
            }
            if(iommu_md::now_ns()-last_progress_ns>50000 || iommu_md::now_ns()-start_ns>1000000) {
                top.md_trace.diagnostic();
                for(unsigned d=0;d<N;++d)
                    for(unsigned dir=0;dir<2;++dir)
                        for(unsigned c=0;c<C;++c) {
                            auto& q=devices[d].q[dir][c];
                            std::cerr << "[NVRR_QUEUE] dir=" << (dir?"W":"R") << " did=" << d
                                      << " q=" << c << " bursts=" << q.bursts.size()
                                      << " popped=" << q.popped_pkts << "/" << q.pushed_pkts
                                      << " done=" << q.done << '\n';
                        }
                std::cerr << "[NVRR_STATE] responded=" << total_responded
                          << " sent_r=" << ds[0].sent << " sent_w=" << ds[1].sent
                          << " buffer=" << top.cache_sub.get_pt_dedup_buffer()->get_valid_count()
                          << " groups=" << top.prefetch_groups.size()
                          << " ptw=" << top.ptw_active_walks.size() << '\n';
                require(false,"NVMe任务队列轮询仲裁场景超时，未释放在途payload");
            }
        }
    }

public:
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext=nullptr; trans.get_extension(ext);
        require(ext && ext->requester_id<N,"响应缺少有效设备ID");
        auto& r=top.md_trace.request(&trans);
        require(r.epoch==epoch,"收到旧epoch响应");
        top.md_trace.response(trans,ext->requester_id);
        ++total_responded;
    }

    // ------------------------------------------------------------------
    // epoch 调度
    // ------------------------------------------------------------------
    void run_epoch(unsigned e) {
        for(auto& dev:devices) {
            for(unsigned dir=0;dir<2;++dir) {
                dev.seq_ctr[dir]=0; dev.q_cur[dir]=0; dev.injected[dir]=0;
                for(unsigned c=0;c<C;++c) {
                    auto& q=dev.q[dir][c];
                    require(q.bursts.empty(),"切换epoch时队列非空");
                    q.done=false; q.last_seq=-1; q.ready_pkts=0;
                    q.pushed_bursts=q.pushed_pkts=q.popped_pkts=q.peak_bursts=q.full_waits=0;
                    q.full_wait_ns=0;
                }
            }
        }
        for(auto& s:ds) {
            s.sent=s.starve_cnt=s.skip_cnt=s.dev_repeat=s.dq_repeat=0;
            s.dev_max_run=s.dq_max_run=s.q_max_run=0;
            s.first_skip_sent=UINT64_MAX;
            s.dev_max_run_pre=s.dq_max_run_pre=s.q_max_run_pre=0;
            s.dev_run=s.dq_run=s.q_run=0;
            s.last_dev=s.last_dq=-1; s.last_q=0;
            s.starve_ns=0; s.first_send_ns=-1; s.last_send_ns=0;
            s.inject_done=false; s.group_last_idx.clear();
            s.gap_sum=s.gap_cnt=s.gap_max=0; s.order_sample.clear();
            s.empty_sel_cnt=0; s.first_done_sent=UINT64_MAX;
            s.sel_live_cnt=0; s.min_occ_live=UINT64_MAX;
            s.occ_sum=s.occ_cnt=0; s.gen_block_ns=0; s.prefill_ok=false;
        }
        total_responded=0; epoch=e; top.md_trace.progress();
        phase_event.notify(SC_ZERO_TIME);
        while(true) {
            if(ds[0].inject_done && ds[1].inject_done &&
               total_responded==expected_count(e) && top.md_quiescent()) break;
            wait(1,SC_NS);
        }
        // ---- 逐设备聚合校验 + [MDMC_RP](与旧场景同格式, 供 iommu-perf-stats 解析) ----
        for(unsigned d=0;d<N;++d) {
            auto& dev=devices[d];
            uint64_t dev_injected=dev.injected[0]+dev.injected[1], dev_peak=0, dev_waits=0;
            double dev_wait_ns=0; uint64_t dev_bursts=0;
            for(unsigned dir=0;dir<2;++dir)
                for(unsigned c=0;c<C;++c) {
                    auto& q=dev.q[dir][c];
                    require(q.popped_pkts==q.pushed_pkts,"队列内包数入/出不一致");
                    require(q.bursts.empty() && q.done,"epoch结束时队列未排空");
                    dev_peak=std::max<uint64_t>(dev_peak,q.peak_bursts);
                    dev_waits+=q.full_waits; dev_wait_ns+=q.full_wait_ns;
                    dev_bursts+=q.pushed_bursts;
                }
            require(dev_injected==expected_count(e)/N,"设备级注入/期望不一致");
            std::cout << "[MDMC_RP] epoch=" << e << " did=" << d << " injected=" << dev_injected
                      << " chan_peak_queue=" << dev_peak << " full_waits=" << dev_waits
                      << " full_wait_ns=" << dev_wait_ns << " bursts=" << dev_bursts << '\n';
        }
        // ---- 每方向包数闭合 + 交织形态 + 队列均衡 ----
        for(unsigned dir=0;dir<2;++dir) {
            auto& s=ds[dir];
            require(s.sent==expected_dir_count(e,dir),"方向注入包数与期望不一致");
            const uint64_t runs_dev=s.sent?s.sent-s.dev_repeat:0;
            const uint64_t runs_dq =s.sent?s.sent-s.dq_repeat:0;
            const double repeat_pct=s.sent>1?100.0*double(s.dev_repeat)/double(s.sent-1):0.0;
            const double gap_avg=s.gap_cnt?double(s.gap_sum)/double(s.gap_cnt):0.0;
            std::cout << "[NVRR_INTERLEAVE] dir=" << (dir?"W":"R") << " epoch=" << e
                      << " sent=" << s.sent
                      << " dev_max_run=" << s.dev_max_run
                      << " dev_avg_run=" << (runs_dev?double(s.sent)/double(runs_dev):0.0)
                      << " dev_repeat_pct=" << repeat_pct
                      << " dq_max_run=" << s.dq_max_run
                      << " dq_avg_run=" << (runs_dq?double(s.sent)/double(runs_dq):0.0)
                      << " q_max_run=" << s.q_max_run
                      << " fullsupply_dev_max_run=" << s.dev_max_run_pre
                      << " fullsupply_dq_max_run=" << s.dq_max_run_pre
                      << " fullsupply_q_max_run=" << s.q_max_run_pre
                      << " first_skip_at_pkt=" << (s.first_skip_sent==UINT64_MAX?-1:int64_t(s.first_skip_sent))
                      << " skip_cnt=" << s.skip_cnt
                      << " same_page_gap_avg_pkts=" << gap_avg
                      << " same_page_gap_max_pkts=" << s.gap_max
                      << " first_send=" << s.first_send_ns << " last_send=" << s.last_send_ns << '\n';
            if(e==iommu_md::MEASUREMENT_EPOCH) {
                // ---- [A0] 饱和度量落盘(供 COMPARISON/SCALING 引用) ----
                const int64_t fskip = s.first_skip_sent==UINT64_MAX ? -1 : int64_t(s.first_skip_sent);
                const int64_t fdone = s.first_done_sent==UINT64_MAX ? -1 : int64_t(s.first_done_sent);
                const int64_t minocc= s.min_occ_live==UINT64_MAX ? -1 : int64_t(s.min_occ_live);
                const double  occavg= s.occ_cnt ? double(s.occ_sum)/double(s.occ_cnt) : 0.0;
                const int64_t tailskip = fskip<0 ? 0 : int64_t(s.sent)-fskip;
                std::cout << "[NVRR_SATURATION] dir=" << (dir?"W":"R")
                          << " prefill_ok=" << (s.prefill_ok?1:0)
                          << " mode=" << (UNBOUNDED?"unbounded":"bounded")
                          << " empty_sel=" << s.empty_sel_cnt
                          << " starve_cnt=" << s.starve_cnt
                          << " starve_ns=" << s.starve_ns
                          << " gen_block_ns=" << s.gen_block_ns
                          << " q_min_occ_live=" << minocc
                          << " q_min_occ_samples=" << s.sel_live_cnt
                          << " q_avg_occ=" << occavg
                          << " first_done_at_pkt=" << fdone
                          << " first_skip_at_pkt=" << fskip
                          << " tail_skip_pkts=" << tailskip << '\n';
                // ---- [A0 硬门禁] 仅无界默认模式启用(构造性饱和, 违反即实现缺陷)
                if(UNBOUNDED) {
                    require(s.prefill_ok,"预填充屏障未通过(冷启动可能空选)");
                    require(s.empty_sel_cnt==0,"RR首选队列为空且生成未结束(饱和被破坏)");
                    require(s.starve_cnt==0 && s.starve_ns==0,
                            "本方向出现发送侧饥饿(供给不足)");
                    require(s.gen_block_ns==0,"无界模式下生成进程不应被反压阻塞");
                    require(s.sel_live_cnt==0 || s.min_occ_live>=1,
                            "选中队列出队前占用为 0(空选)");
                    require(fskip<0 || fdone<0 || fskip>=fdone,
                            "空队列跳过发生在队列排空之前(非尾部排空)");
                }
                std::cout << "[NVRR_ORDER] dir=" << (dir?"W":"R") << " first32=" << s.order_sample << '\n';
                // [门禁] 逐包轮询 => 同一设备连续出现应极少(N>=2 时理论为0)
                if(N>=2) require(repeat_pct<2.0,"设备内/设备间未打散: 同设备连续包占比过高");
                // 队列均衡: RR 派发要求每队列任务(突发)数相差不超过1
                uint64_t bmin=UINT64_MAX,bmax=0,psum=0;
                for(unsigned d=0;d<N;++d) {
                    std::string line;
                    for(unsigned c=0;c<C;++c) {
                        auto& q=devices[d].q[dir][c];
                        bmin=std::min<uint64_t>(bmin,q.pushed_bursts);
                        bmax=std::max<uint64_t>(bmax,q.pushed_bursts);
                        psum+=q.popped_pkts;
                        char buf[96];
                        snprintf(buf,sizeof(buf)," q%u[bursts=%lu pkts=%lu peak=%lu full=%lu]",
                                 c,(unsigned long)q.pushed_bursts,(unsigned long)q.popped_pkts,
                                 (unsigned long)q.peak_bursts,(unsigned long)q.full_waits);
                        line+=buf;
                    }
                    std::cout << "[NVRR_QUEUE_BALANCE] dir=" << (dir?"W":"R") << " did=" << d
                              << line << '\n';
                }
                // [均衡门禁] per_queue 下每队列自带 TASKS 任务 => 突数/包数精确相等;
                //   fixed_total 旧口径: 派发游标余数使队列间任务数差值上界 = 2。
                require(psum==s.sent,"队列出队包数合计与方向注入数不一致");
                uint64_t pmin=UINT64_MAX,pmax=0;
                for(unsigned d=0;d<N;++d)
                    for(unsigned c=0;c<C;++c) {
                        const uint64_t v=devices[d].q[dir][c].popped_pkts;
                        pmin=std::min<uint64_t>(pmin,v); pmax=std::max<uint64_t>(pmax,v);
                    }
                const double pkt_dev_pct=pmax?100.0*double(pmax-pmin)/double(pmax):0.0;
                if(PER_QUEUE) {
                    require(bmin==TASKS && bmax==TASKS,
                            "per_queue 下每队列突发数必须精确等于 TASKS");
                    require(pmin==uint64_t(TASKS)*5 && pmax==pmin,
                            "per_queue 下每队列包数必须精确等于 TASKS*5");
                } else {
                    require(bmax-bmin<=2,"队列间任务数不均衡(RR派发失效)");
                    require(pkt_dev_pct<10.0,"队列间包数偏差过大(通道负载不均)");
                }
                std::cout << "[NVRR_BALANCE_OK] dir=" << (dir?"W":"R")
                          << " bursts_min=" << bmin << " bursts_max=" << bmax
                          << " pkts_min=" << pmin << " pkts_max=" << pmax
                          << " pkt_dev_pct=" << pkt_dev_pct
                          << " pkts_total=" << psum << '\n';
            }
        }
        require(total_responded==expected_count(e),"响应总数与期望总数不一致");
    }

    explicit NvmeRrArbScenario(RP_Module& r) : rp(r),top(*r.iommu_ptr) {}
    ~NvmeRrArbScenario() {
        // 正常结束只在所有响应及异步工作排空后清理；异常停止不提前清理。
        for(auto& trans:payloads) {
            PayloadExtention* ext=nullptr; trans->get_extension(ext);
            trans->clear_extension(ext); delete ext;
            delete[] trans->get_data_ptr(); trans->set_data_ptr(nullptr);
        }
    }
    void run() {
        wait(20,SC_NS);
        start_ns=last_progress_ns=iommu_md::now_ns();
        std::cout << "[MDMC_CONFIG] N=" << N << " C=" << C
                  << " arb=nested_RR(dev_outer,queue_inner,per_packet)"
                  << " senders=2(read_port,write_port) IOVA_MB=" << RANGE_MB
                  << " load_mode=" << (PER_QUEUE?"per_queue":"fixed_total")
                  << " pkts/queue_pair=" << TASKS
                  << " tasks/dev=" << TASKS_DEV << " (read=" << N_READ_TASKS
                  << " write=" << N_WRITE_TASKS << ") pages/dev=" << PAGES_DEV
                  << " pkts/task=" << TASK_PACKETS
                  << " gen_proc/dev=" << C << " bound=queue_pair"
                  << " queue_depth=" << QDEPTH << " qdepth_mode=" << (UNBOUNDED?"unbounded":"bounded")
                  << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " rr_start=" << RR_START
                  << " total_meas_pkts=" << expected_count(iommu_md::MEASUREMENT_EPOCH)
                  << " (rd_port=" << expected_dir_count(iommu_md::MEASUREMENT_EPOCH,0)
                  << " wr_port=" << expected_dir_count(iommu_md::MEASUREMENT_EPOCH,1) << ")\n";
        std::cout << "[NVRR_STIMULUS] gen_proc/dev=" << C << " bound=queue_pair"
                  << " dir_queues/dev=" << C << "R+" << C << "W"
                  << " task_order=SQ,DATA8,CQ rw_alternate=strict"
                  << " sq_route=same_index dispatch=queue_pair_locked"
                  << " arb_granularity=1_packet msi=0\n";
        setup();
        validate_pattern();
        for(unsigned d=0;d<N;++d)
            for(unsigned c=0;c<C;++c) sc_spawn([this,d,c](){ generator(d,c); });
        sc_spawn([this](){ sender(false); });   // sender_r: 独占读口
        sc_spawn([this](){ sender(true); });    // sender_w: 独占写口
        sc_spawn([this](){ watchdog(); });
        invalidate_all();
        run_epoch(1); // 同IOVA、不同S1/S2结果的哨兵。
        invalidate_all();
        run_epoch(2); // 预热不进入正式统计。
        // [STAT] 两项指标只统计测量 epoch: 预热结束后 IOMMU 内部 quiescent,
        //   此处重置 4KB 并发窗口与去重 Cache 有效行采样累加器(与场景1/旧场景2-3 同点)。
        top.reset_4kb_concurrency_window();
        top.cache_sub.reset_dedup_valid_line_stats();
        // [STAT] 稳态窗口按"已完成数(哨兵+预热) + 正式 epoch 的 10%~90%"对齐,
        //   与场景1(total*10%/90%)、旧场景2/3 完全同口径。
        {
            const uint64_t done_before=top.iommu_total_completed;
            const uint64_t meas=expected_count(iommu_md::MEASUREMENT_EPOCH);
            top.steady_start_count=done_before+meas*STEADY_STATE_START_PERCENT/100;
            top.steady_end_count  =done_before+meas*STEADY_STATE_END_PERCENT/100;
            std::cout << "[MDMC_STAT_WINDOW] done_before=" << done_before
                      << " meas=" << meas
                      << " steady_start_count=" << top.steady_start_count
                      << " steady_end_count=" << top.steady_end_count << '\n';
        }
        const uint64_t ptw_before=top.ptw_total_completed, ddr_before=top.axi_master_1_read_req_total;
        run_epoch(iommu_md::MEASUREMENT_EPOCH);
        // [供给验证] 注入期内读/写发送进程的饥饿次数与时长:
        //   starve_ns 越接近 0 => RP 侧任务供给越充足(端口从不因缺任务而空等)。
        std::cout << "[MDMC_SUPPLY] R starve_cnt=" << ds[0].starve_cnt
                  << " starve_ns=" << ds[0].starve_ns << " last_send=" << ds[0].last_send_ns
                  << " | W starve_cnt=" << ds[1].starve_cnt
                  << " starve_ns=" << ds[1].starve_ns << " last_send=" << ds[1].last_send_ns << '\n';
        // 报告前缀带 nvme10_rr, 与旧场景 CSV(multidev_mc_rwsplit_nvme10_n8_c*)完全隔离。
        const std::string pfx = "multidev_mc_rwsplit_nvme10_rr_n" + std::to_string(N)
                                + "_c" + std::to_string(C)
                                + (PER_QUEUE ? "_pq" : "_ft")
                                + "_" + std::to_string(RANGE_MB) + "mb";
        top.md_trace.report(pfx);
        std::cout << "[MDMC_DELTA] PTW=" << top.ptw_total_completed-ptw_before
                  << " DDR_reads=" << top.axi_master_1_read_req_total-ddr_before << '\n';
        // 与场景1/旧场景2-3 同口径的 IOMMU 侧统计，便于横向对比。
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
            printf("======================================================\n");
        }
        top.print_cache_statistics();
        // [稳态窗口] 输出"仅稳态窗口"口径的全量指标(与上面全程口径并列)
        top.print_steady_window_report();
        stopped=true; phase_event.notify(SC_ZERO_TIME);
        for(auto& dev:devices)
            for(unsigned dir=0;dir<2;++dir)
                for(unsigned c=0;c<C;++c) dev.q[dir][c].space_event.notify(SC_ZERO_TIME);
        dir_ready[0].notify(SC_ZERO_TIME); dir_ready[1].notify(SC_ZERO_TIME);
        std::cout << "[NVRR_RESULT] PASS expected=" << expected_count(iommu_md::MEASUREMENT_EPOCH)
                  << " completed=" << total_responded
                  << " rd_port_pkts=" << ds[0].sent << " wr_port_pkts=" << ds[1].sent << "\n";
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario=std::make_shared<NvmeRrArbScenario>(*this);
    md_response_handler=[scenario](tlm_generic_payload& trans){ scenario->response(trans); };
    scenario->run();
}
#endif
