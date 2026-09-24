#include "cache/dc_cache.h"

namespace iommu {

DCCache::DCCache(sc_module_name name, const CacheConfig& cfg,
                 StatsCollector& stats)
    : CacheBase<DCTag, DCData>(name, cfg, stats, "dc_cache")
{
}

bool DCCache::lookup_dc(device_id_t device_id, DCData& out_data, sc_time& latency) {
    DCTag tag;
    tag.device_id = device_id;

    return lookup(tag, out_data, latency);
}

void DCCache::fill_dc(device_id_t device_id, const DCData& data) {
    DCTag tag;
    tag.device_id = device_id;
    fill(tag, data);
}

std::vector<DCCache::InvalidatedContext> DCCache::invalidate_ddt(device_id_t device_id,
                                                                 sc_time* latency) {
    std::vector<InvalidatedContext> contexts;

    DCTag hash_tag;
    hash_tag.device_id = device_id;
    invalidate_precise_by_line(
        hash_tag,
        [device_id](const DCTag& tag, const DCData&) {
            return tag.device_id == device_id;
        },
        [&contexts](const DCTag&, const DCData& data) {
            contexts.push_back({dc_gscid(data), dc_pscid(data)});
        },
        latency);

    return contexts;
}

uint32_t DCCache::invalidate_global(sc_time* latency) {
    return invalidate_all_entries(latency);
}

uint32_t DCCache::hash_function(const DCTag& tag) const {
    uint32_t mask = num_sets_ - 1;
    // [FIX] 原实现 ((did>>16)^((did>>8)<<2)) 丢弃 device_id 低8位,
    // 导致 DID<256 的全部设备哈希到 set 0 (N=8 时 8 上下文挤 4 路产生替换抖动)。
    // 现混入低8位(devfn)与 bus 位: 小 DID 分散到不同 set, 大 RID 仍保持混合。
    uint32_t did = tag.device_id;
    return ((did >> 16) ^ (did >> 8) ^ (did << 2)) & mask;
}

} // namespace iommu
