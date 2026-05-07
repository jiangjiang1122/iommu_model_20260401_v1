#ifndef IOMMU_CACHE_LINE_H
#define IOMMU_CACHE_LINE_H

#include "common/types.h"

namespace iommu {

// ============================================================
// 各 Cache 的 Tag 类型定义
// ============================================================

struct DCTag {
    device_id_t device_id = 0;
    bool operator==(const DCTag& o) const { return device_id == o.device_id; }
};

struct PCTag {
    device_id_t  device_id  = 0;
    process_id_t process_id = 0;
    bool operator==(const PCTag& o) const {
        return device_id == o.device_id && process_id == o.process_id;
    }
};

struct MSIPTTag {
    device_id_t device_id = 0;
    uint32_t    msi_index = 0;
    bool operator==(const MSIPTTag& o) const {
        return device_id == o.device_id && msi_index == o.msi_index;
    }
};

struct PTTag {
    gscid_t    gscid     = 0;
    pscid_t    pscid     = 0;
    iova_t     iova      = 0;  // 页对齐后的地址
    TransStage stage     = TransStage::STAGE1_AND_2;
    bool       sv48      = true;
    bool       gstage_x4 = false;
    bool operator==(const PTTag& o) const {
        return gscid == o.gscid && pscid == o.pscid &&
               iova == o.iova && stage == o.stage && sv48 == o.sv48 &&
               gstage_x4 == o.gstage_x4;
    }
};

struct WalkerTag {
    gscid_t  gscid        = 0;
    pscid_t  pscid        = 0;
    iova_t   va_segment   = 0;  // VA 的切分字段
    uint8_t  level        = 0;  // 页表级别 (1/2/3)
    bool     va_pa_flag   = true;
    bool     stage_flag   = true;
    bool     sv48_flag    = true;
    bool     x4_mode_flag = false;
    bool operator==(const WalkerTag& o) const {
        return gscid == o.gscid && pscid == o.pscid &&
               va_segment == o.va_segment && level == o.level &&
               va_pa_flag == o.va_pa_flag &&
               stage_flag == o.stage_flag &&
               sv48_flag == o.sv48_flag &&
               x4_mode_flag == o.x4_mode_flag;
    }
};

// ============================================================
// 通用 CacheLine 模板
// ============================================================

template <typename TagT, typename DataT>
struct CacheLine {
    bool    valid   = false;
    TagT    tag;
    DataT   data;
    bool    from_prefetch = false;  // 是否由预取填充
    uint64_t access_count = 0;     // 访问计数（用于统计）

    void invalidate() {
        valid = false;
        from_prefetch = false;
        access_count = 0;
    }

    void fill(const TagT& t, const DataT& d, bool prefetched = false) {
        valid = true;
        tag = t;
        data = d;
        from_prefetch = prefetched;
        access_count = 0;
    }
};

// 各 Cache 的 CacheLine 特化类型别名
using DCCacheLine     = CacheLine<DCTag, DCData>;
using PCCacheLine     = CacheLine<PCTag, PCData>;
using MSIPTCacheLine  = CacheLine<MSIPTTag, MSIPTData>;
using PTCacheLine     = CacheLine<PTTag, PTData>;
using WalkerCacheLine = CacheLine<WalkerTag, WalkerData>;

} // namespace iommu

#endif // IOMMU_CACHE_LINE_H
