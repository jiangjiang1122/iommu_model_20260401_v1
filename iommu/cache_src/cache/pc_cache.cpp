#include "cache/pc_cache.h"

namespace iommu {

PCCache::PCCache(sc_module_name name, const CacheConfig& cfg,
                 StatsCollector& stats)
    : CacheBase<PCTag, PCData>(name, cfg, stats, "pc_cache")
{
}

bool PCCache::lookup_pc(device_id_t device_id, process_id_t process_id,
                        PCData& out_data, sc_time& latency) {
    PCTag tag;
    tag.device_id = device_id;
    tag.process_id = process_id;
    return lookup(tag, out_data, latency);
}

void PCCache::fill_pc(device_id_t device_id, process_id_t process_id,
                      const PCData& data) {
    PCTag tag;
    tag.device_id = device_id;
    tag.process_id = process_id;
    fill(tag, data);
}

std::vector<PCCache::InvalidatedContext> PCCache::invalidate_pdt(
    device_id_t device_id, process_id_t process_id, bool has_process_id,
    sc_time* latency) {
    std::vector<InvalidatedContext> contexts;

    PCTag hash_tag;
    hash_tag.device_id = device_id;
    hash_tag.process_id = has_process_id ? process_id : 0;

    auto predicate = [=](const PCTag& tag, const PCData&) {
        if (tag.device_id != device_id) return false;
        if (has_process_id && tag.process_id != process_id) return false;
        return true;
    };
    auto on_invalidated = [&contexts](const PCTag&, const PCData& data) {
        contexts.push_back({0, pc_pscid(data)});
    };

    if (has_process_id) {
        invalidate_precise_by_line(hash_tag, predicate, on_invalidated, latency);
    } else {
        invalidate_scan_by_line(predicate, on_invalidated, latency);
    }

    return contexts;
}

uint32_t PCCache::invalidate_global(sc_time* latency) {
    return invalidate_all_entries(latency);
}

uint32_t PCCache::hash_function(const PCTag& tag) const {
    uint32_t mask = num_sets_ - 1;
    uint32_t temp1 = tag.device_id ^ (tag.device_id >> 8) ^ (tag.device_id >> 16);
    uint32_t temp2 = tag.process_id ^ (tag.process_id >> 10);
    return (temp1 ^ temp2) & mask;
}

} // namespace iommu
