#ifndef IOMMU_DC_CACHE_H
#define IOMMU_DC_CACHE_H

#include "cache/cache_base.h"

namespace iommu {

class DCCache : public CacheBase<DCTag, DCData> {
public:
    SC_HAS_PROCESS(DCCache);

    DCCache(sc_module_name name, const CacheConfig& cfg,
            StatsCollector& stats);

    // DC Cache 特有接口
    bool lookup_dc(device_id_t device_id, DCData& out_data, sc_time& latency);
    void fill_dc(device_id_t device_id, const DCData& data);

    // 失效: iodir.inval_ddt
    // 返回被失效entry的 {gscid, pscid} 列表用于级联
    struct InvalidatedContext {
        gscid_t gscid;
        pscid_t pscid;
    };
    std::vector<InvalidatedContext> invalidate_ddt(device_id_t device_id,
                                                   sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const DCTag& tag) const override;
};

} // namespace iommu

#endif // IOMMU_DC_CACHE_H
