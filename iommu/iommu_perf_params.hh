#ifndef __IOMMU_PERF_PARAMS_HH__
#define __IOMMU_PERF_PARAMS_HH__

#include <stdint.h>

// ===================== FIFO深度参数 =====================
// Parser输出
static const uint32_t FIFO_DEPTH_PARSER_TO_COLLECTOR = 256;
static const uint32_t FIFO_DEPTH_PARSER_TO_PQ = 256;
static const uint32_t FIFO_DEPTH_PARSER_TO_DC_CACHE_QUERY = 256;
static const uint32_t FIFO_DEPTH_PARSER_TO_PC_CACHE_QUERY = 256;

// Collector到Cache
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE = 256;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE = 256;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_PT_CACHE_QUERY = 256;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_MSIPT_CACHE_QUERY = 256;

// Collector到Walker
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_XDTW = 128;

// Cache到Walker
static const uint32_t FIFO_DEPTH_PT_CACHE_TO_PTW = 256;
static const uint32_t FIFO_DEPTH_MSIPT_CACHE_TO_MSIPTW = 64;

// Walker返回Collector/Cache
static const uint32_t FIFO_DEPTH_XDTW_TO_COLLECTOR = 128;
static const uint32_t FIFO_DEPTH_PTW_TO_PT_CACHE = 256;
static const uint32_t FIFO_DEPTH_MSIPTW_TO_MSIPT_CACHE = 64;

// Cache返回Collector
static const uint32_t FIFO_DEPTH_DC_CACHE_TO_COLLECTOR = 256;
static const uint32_t FIFO_DEPTH_PC_CACHE_TO_COLLECTOR = 256;

// 最终输出路径
static const uint32_t FIFO_DEPTH_PT_CACHE_TO_FWD = 256;
static const uint32_t FIFO_DEPTH_MSIPT_CACHE_TO_FWD = 256;
static const uint32_t FIFO_DEPTH_COLLECTOR_TO_FAULT = 256;

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
static const uint32_t FIFO_DEPTH_INBOUND = 256;

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
static const uint32_t DDR_MAX_OUTSTANDING = 512;    // 最大未完成请求数，必须≥所有walker outstanding之和
static const uint32_t DDR_READ_LATENCY = 10;        // DDR读延迟(ns)
static const uint32_t DDR_WRITE_LATENCY = 8;        // DDR写延迟(ns)
static const uint32_t DDR_BANDWIDTH_GBPS = 128;     // DDR带宽(GB/s)

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

// ===================== 系统参数 =====================
static const uint32_t MAX_CONCURRENT_TASKS = 256;   // 最大并发任务数
static const uint32_t MAX_DEVICES = 1024;           // 最大设备数
static const uint32_t MAX_PROCESSES = 65536;        // 最大进程数
static const uint32_t MAX_DEVID_WIDTH = 24;         // 设备ID最大位宽

#endif // __IOMMU_PERF_PARAMS_HH__