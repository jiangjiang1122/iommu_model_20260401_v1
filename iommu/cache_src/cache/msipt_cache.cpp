#include "cache/msipt_cache.h"

namespace iommu {

MSIPTCache::MSIPTCache(sc_module_name name, const CacheConfig& cfg,
                       StatsCollector& stats)
    : CacheBase<MSIPTTag, MSIPTData>(name, cfg, stats, "msipt_cache")
{
}

bool MSIPTCache::lookup_msi(device_id_t device_id, uint32_t msi_index,
                            MSIPTData& out_data, sc_time& latency) {
    MSIPTTag tag;
    tag.device_id = device_id;
    tag.msi_index = msi_index;
    return lookup(tag, out_data, latency);
}

void MSIPTCache::fill_msi(device_id_t device_id, uint32_t msi_index,
                          const MSIPTData& data) {
    MSIPTTag tag;
    tag.device_id = device_id;
    tag.msi_index = msi_index;
    fill(tag, data);
}

uint32_t MSIPTCache::invalidate_by_device(device_id_t device_id, sc_time* latency) {
    return invalidate_scan_by_line(
        [device_id](const MSIPTTag& tag, const MSIPTData&) {
            return tag.device_id == device_id;
        },
        [](const MSIPTTag&, const MSIPTData&) {},
        latency);
}

uint32_t MSIPTCache::invalidate_global(sc_time* latency) {
    return invalidate_all_entries(latency);
}

uint32_t MSIPTCache::hash_function(const MSIPTTag& tag) const {
    // 与 DC Cache 类似的散列策略
    uint32_t mask = num_sets_ - 1;
    uint8_t bus = get_bus(tag.device_id);
    uint8_t func = get_function(tag.device_id);
    uint32_t func_padded = static_cast<uint32_t>(func) << 2;
    uint32_t bus_low = static_cast<uint32_t>(bus) & mask;
    uint32_t func_low = func_padded & mask;
    uint32_t msi_low = tag.msi_index & mask;

    return (bus_low ^ func_low ^ msi_low) & mask;
}

} // namespace iommu
