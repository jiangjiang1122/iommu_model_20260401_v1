#ifndef IOMMU_DEDUP_PARAMS_HH
#define IOMMU_DEDUP_PARAMS_HH

#include <cstdint>

// ============================================================
// Buffer配置参数
// ============================================================

/// Dedup Buffer容量 (最大任务链表entry数)
static constexpr uint32_t DEDUP_BUFFER_CAPACITY = 256;

/// Buffer满时的反压策略: true=阻塞等待, false=丢弃请求
static constexpr bool DEDUP_BUFFER_BACKPRESSURE = true;

// ============================================================
// PT Cache配置参数
// ============================================================

/// PT Cache占位CL的is_ph标志位位置
static constexpr uint32_t PT_RESERVED_IS_PH_BIT = 7;

/// PT Cache占位CL的is_req标志位位置
static constexpr uint32_t PT_RESERVED_IS_REQ_BIT = 17;

/// Buffer链表尾标记 (0xFF=无下一项)
static constexpr uint8_t BUFFER_CHAIN_END = 0xFF;

// ============================================================
// 预取配置参数
// ============================================================

/// 默认预取深度 (D值)
/// D=0: 预取功能关闭
/// D>0: 预取功能启用,预取深度为D
static constexpr uint32_t DEFAULT_PREFETCH_DEPTH = 8;

/// 预取是否默认启用
static constexpr bool PREFETCH_ENABLED_BY_DEFAULT = true;

/// Burst读取的最大PTE数 (防止跨页表边界)
static constexpr uint32_t MAX_BURST_PTE_COUNT = 64;

// ============================================================
// PTE有效性标记
// ============================================================

/// PTE有效标志 (V位)
static constexpr uint64_t PTE_VALID_MASK = 0x1;

/// PTE无效/错误标记 (用于ptw_response_t)
enum class PTEStatus : uint8_t {
    PTE_VALID = 0,          ///< 正常有效PTE
    PTE_INVALID = 1,        ///< 无效PTE (V=0)
    PTE_ERROR = 2,          ///< 错误PTE (权限错误等)
    PTE_RESERVED = 3        ///< 保留
};

// ============================================================
// 页大小配置
// ============================================================

/// 4KB页大小 (字节)
static constexpr uint64_t PAGE_SIZE_4KB = 0x1000;

/// 4KB页掩码
static constexpr uint64_t PAGE_MASK_4KB = 0xFFF;

/// 页表页大小 (4KB, 存储512个PTE)
static constexpr uint64_t PT_PAGE_SIZE = 0x1000;

/// 页表页掩码
static constexpr uint64_t PT_PAGE_MASK = 0xFFF;

// ============================================================
// Sv39地址翻译参数
// ============================================================

/// Sv39 VPN位数
static constexpr uint32_t SV39_VPN_BITS = 9;

/// Sv39 PPN位数
static constexpr uint32_t SV39_PPN_BITS = 26;

/// Sv39页表级数
static constexpr uint32_t SV39_LEVELS = 3;

/// PTE大小 (Sv39为8字节)
static constexpr uint32_t PTE_SIZE = 8;

#endif // IOMMU_DEDUP_PARAMS_HH
