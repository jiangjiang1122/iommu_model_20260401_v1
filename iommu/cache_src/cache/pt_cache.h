#ifndef IOMMU_PT_CACHE_H
#define IOMMU_PT_CACHE_H

#include "cache/cache_base.h"

namespace iommu {

class PTCache : public CacheBase<PTTag, PTData> {
public:
    SC_HAS_PROCESS(PTCache);

    PTCache(sc_module_name name, const CacheConfig& cfg,
            StatsCollector& stats);

    ~PTCache() override = default;

    // PT Cache 特有接口
    bool lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                   TransStage stage, bool sv48, bool gstage_x4,
                   PTData& out_data, sc_time& latency);
    bool lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                   TransStage stage, PTData& out_data, sc_time& latency) {
        return lookup_pt(gscid, pscid, iova, stage, true, false, out_data, latency);
    }

    void fill_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                 TransStage stage, const PTData& data, bool from_prefetch = false);

    // 失效操作
    // IOTINVAL.VMA: 第一阶段页表失效
    uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                            bool has_gscid, bool has_pscid, bool has_iova,
                            CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                            sc_time* latency = nullptr);

    // IOTINVAL.GVMA: 第二阶段页表失效
    uint32_t invalidate_gvma(gscid_t gscid, iova_t gpa,
                             bool has_gscid, bool has_gpa,
                             CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                             sc_time* latency = nullptr);

    // 按 gscid/pscid 批量失效 (级联用)
    uint32_t invalidate_by_gscid(gscid_t gscid, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                       sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const PTTag& tag) const override;

private:
    // 将 iova 按页大小对齐
    static iova_t align_iova(iova_t iova, PageSize ps);
};

} // namespace iommu

#endif // IOMMU_PT_CACHE_H
