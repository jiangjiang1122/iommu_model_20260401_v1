#ifndef IOMMU_PC_CACHE_H
#define IOMMU_PC_CACHE_H

#include "cache/cache_base.h"

namespace iommu {

class PCCache : public CacheBase<PCTag, PCData> {
public:
    SC_HAS_PROCESS(PCCache);

    PCCache(sc_module_name name, const CacheConfig& cfg,
            StatsCollector& stats);

    // PC Cache 特有接口
    bool lookup_pc(device_id_t device_id, process_id_t process_id,
                   PCData& out_data, sc_time& latency);
    void fill_pc(device_id_t device_id, process_id_t process_id,
                 const PCData& data);

    // 失效: iodir.inval_pdt
    struct InvalidatedContext {
        gscid_t gscid;
        pscid_t pscid;
    };
    std::vector<InvalidatedContext> invalidate_pdt(
        device_id_t device_id, process_id_t process_id, bool has_process_id,
        sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const PCTag& tag) const override;
};

} // namespace iommu

#endif // IOMMU_PC_CACHE_H
