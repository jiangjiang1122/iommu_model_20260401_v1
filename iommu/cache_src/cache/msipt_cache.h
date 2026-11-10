#ifndef IOMMU_MSIPT_CACHE_H
#define IOMMU_MSIPT_CACHE_H

#include "cache/cache_base.h"

namespace iommu {

class MSIPTCache : public CacheBase<MSIPTTag, MSIPTData> {
public:
    SC_HAS_PROCESS(MSIPTCache);

    MSIPTCache(sc_module_name name, const CacheConfig& cfg,
               StatsCollector& stats);

    // MSIPT Cache 特有接口
    bool lookup_msi(device_id_t device_id, uint32_t msi_index,
                    MSIPTData& out_data, sc_time& latency);
    void fill_msi(device_id_t device_id, uint32_t msi_index,
                  const MSIPTData& data);

    // 失效
    uint32_t invalidate_by_device(device_id_t device_id, sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const MSIPTTag& tag) const override;
};

} // namespace iommu

#endif // IOMMU_MSIPT_CACHE_H
