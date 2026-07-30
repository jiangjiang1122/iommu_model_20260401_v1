#ifndef __IOMMU_PERF_PARAMS_HH__
#define __IOMMU_PERF_PARAMS_HH__

#include <stdint.h>

// ===================== FIFO深度参数 =====================
// Parser输出
static const uint32_t FIFO_DEPTH_PARSER_TO_COLLECTOR = 4;
static const uint32_t FIFO_DEPTH_PARSER_TO_PQ = 4;
static const uint32_t FIFO_DEPTH_PARSER_TO_DC_CACHE_QUERY = 4;
static const uint32_t FIFO_DEPTH_PARSER_TO_PC_CACHE_QUERY = 4;

// Collector到Cache
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE = 4;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE = 4;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_PT_CACHE_QUERY = 4;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_MSIPT_CACHE_QUERY = 4;

// Collector到Walker
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_XDTW = 4;

// Cache到Walker
static const uint32_t FIFO_DEPTH_PT_CACHE_TO_PTW = 256;
static const uint32_t FIFO_DEPTH_MSIPT_CACHE_TO_MSIPTW = 4;

// Walker返回Collector/Cache
static const uint32_t FIFO_DEPTH_XDTW_TO_COLLECTOR = 4;
static const uint32_t FIFO_DEPTH_PTW_TO_PT_CACHE = 32;
static const uint32_t FIFO_DEPTH_MSIPTW_TO_MSIPT_CACHE = 4;

// Cache返回Collector
static const uint32_t FIFO_DEPTH_DC_CACHE_TO_COLLECTOR = 4;
static const uint32_t FIFO_DEPTH_PC_CACHE_TO_COLLECTOR = 4;

// 最终输出路径
static const uint32_t FIFO_DEPTH_PT_CACHE_TO_FWD = 32;
static const uint32_t FIFO_DEPTH_MSIPT_CACHE_TO_FWD = 4;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_FAULT = 4;

// DDR FIFO深度
static const uint32_t FIFO_DEPTH_DDR_RSP = 256;
static const uint32_t FIFO_DEPTH_XDTW_REQ_DDR = 128;
static const uint32_t FIFO_DEPTH_XDTW_RSP_DDR = 128;
static const uint32_t FIFO_DEPTH_PTW_REQ_DDR = 256;
static const uint32_t FIFO_DEPTH_PTW_RSP_DDR = 256;
static const uint32_t FIFO_DEPTH_MSIPTW_REQ_DDR = 64;
static const uint32_t FIFO_DEPTH_MSIPTW_RSP_DDR = 64;
static const uint32_t FIFO_DEPTH_CTRL_PATH_REQ_DDR = 32;

// 入站缓冲
static const uint32_t FIFO_DEPTH_INBOUND = 4;

// ===================== 缓存参数 =====================
// DC Cache (Device Context Cache)
static const uint32_t DC_CACHE_SIZE = 64;           // 64条目
static const uint32_t DC_CACHE_ASSOC = 8;           // 8路组相联
static const uint32_t DC_CACHE_LINE_SIZE = 64;      // 64字节缓存行

// PC Cache (Process Context Cache)  
static const uint32_t PC_CACHE_SIZE = 32;           // 32条目
static const uint32_t PC_CACHE_ASSOC = 4;           // 4路组相联
static const uint32_t PC_CACHE_LINE_SIZE = 32;      // 32字节缓存行

// PT Cache (Page Table Cache, IOTLB)
static const uint32_t PT_CACHE_SIZE = 256;          // 256条目
static const uint32_t PT_CACHE_ASSOC = 16;          // 16路组相联
static const uint32_t PT_CACHE_LINE_SIZE = 16;      // 16字节缓存行

// MSIPT Cache (MSI Page Table Cache)
static const uint32_t MSIPT_CACHE_SIZE = 16;        // 16条目
static const uint32_t MSIPT_CACHE_ASSOC = 4;        // 4路组相联
static const uint32_t MSIPT_CACHE_LINE_SIZE = 16;   // 16字节缓存行

// ===================== AXI ID参数 =====================
static const uint16_t MAX_AXI_IDS = 256;            // 最大AXI ID数量
static const uint16_t AXI_ID_POOL_INIT_SIZE = 64;   // 初始可用ID池大小

// ===================== DDR性能参数 =====================
static const uint32_t DDR_MAX_OUTSTANDING = 512;    // 最大未完成请求数，必须≥所有walker outstanding之和（建议值：≥448）
static const uint32_t DDR_READ_LATENCY = 10;        // DDR读延迟(ns)
static const uint32_t DDR_WRITE_LATENCY = 8;        // DDR写延迟(ns)
static const uint32_t DDR_BANDWIDTH_GBPS = 128;     // DDR带宽(GB/s)

// ===================== AXI Master端口并发限制 =====================
static const uint32_t AXI_MASTER_1_TO_CMN_RND_MAX_OUTSTANDING = 256;  // axi_master_1_to_cmn_rnd_socket并发任务数（DDR访问）
static const uint32_t AXI_MASTER_0_TO_PCIE_NOC_MAX_OUTSTANDING = 256; // axi_master_0_to_pcie_noc_to_cmn_rni_socket并发任务数（DMA/RP访问）

// ===================== 端口带宽参数 (bandwidth_mbps = freq_mhz * width_bit) =====================
// 带宽延迟公式: delay_ns = 1000.0 * data_length_bytes * 8 / bandwidth_mbps
static const uint32_t AXI_SLAVE_0_FREQ_MHZ  = 1000;   // axi_slave_from_pcie_noc_0_socket 频率(MHz)
// [场景化] 入口/出口端口位宽: 默认512bit=64GB/s(场景5); 场景6经Makefile传入1024bit=128GB/s
#ifndef TEST_CFG_AXI_PORT_WIDTH_BIT
static const uint32_t AXI_SLAVE_0_WIDTH_BIT  = 512;    // axi_slave_from_pcie_noc_0_socket 数据位宽(bit)
#else
static const uint32_t AXI_SLAVE_0_WIDTH_BIT  = TEST_CFG_AXI_PORT_WIDTH_BIT;
#endif
static const uint32_t AXI_SLAVE_0_BANDWIDTH_MBPS = AXI_SLAVE_0_FREQ_MHZ * AXI_SLAVE_0_WIDTH_BIT;  // 512bit=64GB/s, 1024bit=128GB/s

static const uint32_t AXI_MASTER_0_FREQ_MHZ = 1000;   // axi_master_0_to_pcie_noc 频率(MHz)
static const uint32_t AXI_MASTER_0_WIDTH_BIT = AXI_SLAVE_0_WIDTH_BIT;  // 出口位宽与入口一致
static const uint32_t AXI_MASTER_0_BANDWIDTH_MBPS = AXI_MASTER_0_FREQ_MHZ * AXI_MASTER_0_WIDTH_BIT;

static const uint32_t AXI_MASTER_1_FREQ_MHZ = 1000;   // axi_master_1_to_cmn_rnd 频率(MHz)
static const uint32_t AXI_MASTER_1_WIDTH_BIT = 128;    // axi_master_1_to_cmn_rnd 数据位宽(bit)
static const uint32_t AXI_MASTER_1_BANDWIDTH_MBPS = AXI_MASTER_1_FREQ_MHZ * AXI_MASTER_1_WIDTH_BIT;  // 128000 Mbps = 16GB/s

// ===================== Walker Outstanding 限制 =====================
static const uint32_t XDTW_MAX_DC_OUTSTANDING_TASKS = 64;   // xDTW DC(DDT) walk outstanding
static const uint32_t XDTW_MAX_PC_OUTSTANDING_TASKS = 64;   // xDTW PC(PDT) walk outstanding
// [场景化] PTW并发度: 默认4(场景5, 64GB/s够用); 场景6经Makefile传入5
//   (128GB/s下达标250M所需最小并发: 5组x32请求/554ns=289M > 250M)
#ifndef TEST_CFG_PTW_MAX_OUTSTANDING_TASKS
static const uint32_t PTW_MAX_OUTSTANDING_TASKS = 4;          // PTW总outstanding任务数（按主任务计数，每组=1主+D预取）
#else
static const uint32_t PTW_MAX_OUTSTANDING_TASKS = TEST_CFG_PTW_MAX_OUTSTANDING_TASKS;
#endif
static const uint32_t PTW_REQ_PIPELINE_DELAY_NS = 30;         // PTW请求流水延时(ns, PEQ)
static const uint32_t PTW_RSP_PIPELINE_DELAY_NS = 2;         // PTW响应流水延时(ns, PEQ)
static const uint32_t MSIPTW_MAX_OUTSTANDING_TASKS = 64;     // MSIPTW总outstanding任务数

// ===================== PTW模块参数 =====================
// Walker Cache开关（true=启用，false=禁用）
// 允许通过Makefile TEST_FLAGS传入 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=0 覆盖
#ifndef TEST_CFG_PTW_WALKER_CACHE_ENABLED
static const bool PTW_WALKER_CACHE_ENABLED = true;
#else
static const bool PTW_WALKER_CACHE_ENABLED = TEST_CFG_PTW_WALKER_CACHE_ENABLED;
#endif

// Walker Cache S2 Cache开关（两阶段翻译中G-stage显式第二阶段的walker cache）
// true=启用，false=禁用（禁用时为现有基线状态）
#ifndef TEST_CFG_WALKER_CACHE_S2_ENABLED
static const bool PTW_WALKER_S2_CACHE_ENABLED = true;
#else
static const bool PTW_WALKER_S2_CACHE_ENABLED = TEST_CFG_WALKER_CACHE_S2_ENABLED;
#endif

// [前置] Walker Cache前置查询开关：true=所有输入请求在PT Cache查询发起点同时
// 查询Walker Cache, 结果随任务透传至PTW直接使用(PTW不再自行查询);
// false=恢复旧路径(PTW内同步查询), 用于基线A/B回归验证
#ifndef TEST_CFG_WALKER_FRONT_ENABLED
static const bool WALKER_FRONT_ENABLED = true;
#else
static const bool WALKER_FRONT_ENABLED = TEST_CFG_WALKER_FRONT_ENABLED;
#endif

// [前置] PTW二次校验开关: 前置查询MISS的任务到达PTW时再查一次Walker Cache
// (高带宽顺序场景下, 同一页表段的一批任务在首任务walk完成前涌入,
// 前置结果均为MISS, 到PTW时Walker已被更新可命中; 二次校验恢复基线行为)
// 前置HIT的任务不二次查询。置0=纯严格前置语义
#ifndef TEST_CFG_WALKER_FRONT_RECHECK
static const bool WALKER_FRONT_RECHECK = true;
#else
static const bool WALKER_FRONT_RECHECK = TEST_CFG_WALKER_FRONT_RECHECK;
#endif

// ===================== PT Cache VA去重参数 =====================
static const bool PT_CACHE_VA_DEDUP_ENABLED = false;         // VA去重功能开关（true=启用，false=禁用）

// ===================== PT Cache去重+预取模块参数 =====================
static const bool PT_CACHE_DEDUP_ENABLED = true;             // 去重功能开关
// Buffer必须 >= IOMMU_GLOBAL_MAX_OUTSTANDING: 否则挂起任务可致Buffer满,
// RAM Worker阻塞 -> RAM FIFO满 -> Hash/Scheduler阻塞 -> UPDATE无法分发 -> 死锁环
// [场景化] 跟随全局并发上限: 场景5=256, 场景6=512
#ifndef TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING
static const uint32_t PT_DEDUP_BUFFER_SIZE = 256;            // Buffer大小（entries）
#else
static const uint32_t PT_DEDUP_BUFFER_SIZE = TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING;
#endif
// 预取深度（页数量，D=0表示关闭预取）
// 允许通过Makefile TEST_FLAGS传入 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=0 覆盖
#ifndef TEST_CFG_PT_DEDUP_PREFETCH_DEPTH
static const uint32_t PT_DEDUP_PREFETCH_DEPTH = 3;
#else
static const uint32_t PT_DEDUP_PREFETCH_DEPTH = TEST_CFG_PT_DEDUP_PREFETCH_DEPTH;
#endif
static const uint16_t DEDUP_BUFFER_INVALID_IDX = 0xFFFF;     // 无效索引标记（支持512 entries）

// ===================== dedup_cache 流水线时序参数 =====================
// [dedup多RAM] 查询流水线: Scheduler -> Hash(1cyc, dedup_hash_process_thread显式消耗)
//   -> 按ram_id分发 -> RAM原子段[get set -> get free line -> write line](5cyc,
//      dedup_ram_worker_thread消耗, 同RAM串行/跨RAM并发)
// MISS的D个预取占位异步化为独立任务(经dedup_inside_request_fifo), 各消耗1个原子段。
// 稳态吞吐 ≈ num_rams 个结果 / 5cyc (跨RAM并发理想情况)。
static const uint32_t DEDUP_HASH_CYCLES        = 1;   // hash 单元单拍延时
static const uint32_t DEDUP_GET_SET_CYCLES     = 2;   // get cache set (稳态口径, 见需求书注2/3)
static const uint32_t DEDUP_GET_FREE_LINE_CYCLES = 2; // hit? get free cache line
static const uint32_t DEDUP_WRITE_LINE_CYCLES  = 1;   // write cache line
// 原子段总周期 = get_set + get_free_line + write_line = 5cyc
static const uint32_t DEDUP_ATOMIC_CYCLES =
    DEDUP_GET_SET_CYCLES + DEDUP_GET_FREE_LINE_CYCLES + DEDUP_WRITE_LINE_CYCLES;

// ===================== Walk上下文读缓冲区大小 =====================
// 必须同时满足：
//   1) DC/PC读取：最大 EXT_FORMAT_DC_SIZE = 64 字节
//   2) PTW combined burst读取：(1 + D + 1) * 8 字节
// 当 D=0 时 combined burst 为 16 字节，但 DC 读取需要 64 字节，
// 因此缓冲区不能仅由预取深度决定，否则关闭预取时 DC/PC 读取会越界。
static const uint32_t WALK_CTX_READ_BUF_SIZE =
    (((1 + PT_DEDUP_PREFETCH_DEPTH + 1) * 8) > 64) ?
     ((1 + PT_DEDUP_PREFETCH_DEPTH + 1) * 8) : 64;

// ===================== Collector Outstanding 限制 =====================
static const uint32_t COLLECTOR_MAX_DC_WALK_OUTSTANDING = 64;   // Collector DC walk outstanding上限
static const uint32_t COLLECTOR_MAX_PC_WALK_OUTSTANDING = 64;   // Collector PC walk outstanding上限

// ===================== 处理延迟参数 ===================
// 各模块处理延迟(ns)
static const uint32_t PARSER_DELAY = 1;             // Parser解析延迟
static const uint32_t COLLECTOR_DELAY = 1;          // Collector处理延迟
static const uint32_t DC_CACHE_HIT_DELAY = 2;       // DC Cache命中延迟
static const uint32_t PC_CACHE_HIT_DELAY = 2;       // PC Cache命中延迟
static const uint32_t PT_CACHE_HIT_DELAY = 3;       // PT Cache命中延迟
static const uint32_t MSIPT_CACHE_HIT_DELAY = 3;    // MSIPT Cache命中延迟
static const uint32_t XDTW_DELAY_PER_ACCESS = 5;    // xDTW每次DDR访问延迟
static const uint32_t PTW_DELAY_PER_ACCESS = 5;     // PTW每次DDR访问延迟
static const uint32_t MSIPTW_DELAY_PER_ACCESS = 5;  // MSIPTW每次DDR访问延迟
static const uint32_t FORWARDER_DELAY = 2;          // Forwarder转发延迟

// ===================== 出口重排序与全局 Outstanding =====================
// IOMMU 模块整体 outstanding 上限：
//   入口 parser 申请，重排序输出后释放。
// [场景化] 默认256(场景5); 场景6经Makefile传入512(随128GB/s带宽提升)
#ifndef TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING
static const uint32_t IOMMU_GLOBAL_MAX_OUTSTANDING = 256;
#else
static const uint32_t IOMMU_GLOBAL_MAX_OUTSTANDING = TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING;
#endif
// 重排序输出每次发送的延迟(ns)
static const uint32_t REORDER_OUTPUT_DELAY = 1;

// ===================== SLINK NoC延迟参数 =====================
static const uint32_t SLINK_NOC_LATENCY_NS = 50;            // SLINK NoC单趟延时(ns)，50ns（低延时场景）

// ===================== 稳态IOPS采样窗口 =====================
// 跳过前10%和后10%，只统计中间80%稳定段的IOPS
static const uint32_t STEADY_STATE_START_PERCENT = 10;  // 稳态开始百分比
static const uint32_t STEADY_STATE_END_PERCENT   = 90;  // 稳态结束百分比

// ===================== 系统参数 =====================
static const uint32_t MAX_CONCURRENT_TASKS = 256;   // 最大并发任务数
static const uint32_t MAX_DEVICES = 64;           // 最大设备数
static const uint32_t MAX_PROCESSES = 64;        // 最大进程数
static const uint32_t MAX_DEVID_WIDTH = 24;         // 设备ID最大位宽


#endif // __IOMMU_PERF_PARAMS_HH__