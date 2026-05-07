#include "cache/pt_cache.h"

namespace iommu {

PTCache::PTCache(sc_module_name name, const CacheConfig& cfg,
                 StatsCollector& stats)
    : CacheBase<PTTag, PTData>(name, cfg, stats, "pt_cache")
{
}

bool PTCache::lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                        TransStage stage, bool sv48, bool gstage_x4,
                        PTData& out_data, sc_time& latency) {
    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);  // 页对齐，与fill_pt保持一致
    tag.stage = stage;
    tag.sv48 = sv48;
    tag.gstage_x4 = gstage_x4;

    return lookup(tag, out_data, latency);
}

void PTCache::fill_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                      TransStage stage, const PTData& data, bool from_prefetch) {
    PTData stored_data = data;
    stored_data.reserved.trans_type = static_cast<uint32_t>(stage);
    stored_data.reserved.iova_is_va = (stage == TransStage::STAGE2_ONLY) ? 0U : 1U;

    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);
    tag.stage = stage;
    tag.sv48 = pt_sv48_mode(stored_data);
    tag.gstage_x4 = pt_gstage_x4_mode(stored_data);
    fill(tag, stored_data, from_prefetch);
}

uint32_t PTCache::invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                                 bool has_gscid, bool has_pscid, bool has_iova,
                                 CacheInvalidateMode mode, sc_time* latency) {
    auto predicate = [=](const PTTag& tag, const PTData&) {
        if (has_gscid && tag.gscid != gscid) return false;
        if (has_pscid && tag.pscid != pscid) return false;
        if (has_iova && tag.iova != align_iova(iova, PageSize::PAGE_4K)) return false;
        // VMA 失效仅影响包含第一阶段的翻译
        if (tag.stage == TransStage::STAGE2_ONLY) return false;
        return true;
    };

    if (mode == CacheInvalidateMode::GLOBAL) {
        return invalidate_all_entries(latency);
    }

    if (mode == CacheInvalidateMode::PRECISE && has_gscid && has_pscid && has_iova) {
        PTTag hash_tag;
        hash_tag.gscid = gscid;
        hash_tag.pscid = pscid;
        hash_tag.iova = align_iova(iova, PageSize::PAGE_4K);
        hash_tag.stage = TransStage::STAGE1_AND_2;
        return invalidate_precise_by_line(
            hash_tag,
            predicate,
            [](const PTTag&, const PTData&) {},
            latency);
    }

    return invalidate_scan_by_line(
        predicate,
        [](const PTTag&, const PTData&) {},
        latency);
}

uint32_t PTCache::invalidate_gvma(gscid_t gscid, iova_t gpa,
                                  bool has_gscid, bool has_gpa,
                                  CacheInvalidateMode mode, sc_time* latency) {
    auto predicate = [=](const PTTag& tag, const PTData&) {
        if (has_gscid && tag.gscid != gscid) return false;
        if (has_gpa && tag.iova != align_iova(gpa, PageSize::PAGE_4K)) return false;
        // GVMA 失效仅影响包含第二阶段的翻译
        if (tag.stage == TransStage::STAGE1_ONLY) return false;
        return true;
    };

    if (mode == CacheInvalidateMode::GLOBAL) {
        return invalidate_all_entries(latency);
    }

    return invalidate_scan_by_line(
        predicate,
        [](const PTTag&, const PTData&) {},
        latency);
}

uint32_t PTCache::invalidate_by_gscid(gscid_t gscid, sc_time* latency) {
    return invalidate_scan_by_line(
        [gscid](const PTTag& tag, const PTData&) {
            return tag.gscid == gscid;
        },
        [](const PTTag&, const PTData&) {},
        latency);
}

uint32_t PTCache::invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                            sc_time* latency) {
    return invalidate_scan_by_line(
        [gscid, pscid](const PTTag& tag, const PTData&) {
            return tag.gscid == gscid && tag.pscid == pscid;
        },
        [](const PTTag&, const PTData&) {},
        latency);
}

uint32_t PTCache::invalidate_global(sc_time* latency) {
    return invalidate_all_entries(latency);
}

uint32_t PTCache::hash_function(const PTTag& tag) const {
    uint32_t mask = num_sets_ - 1;
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 44) - 1;
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    __uint128_t array =
        (static_cast<__uint128_t>(tag.gscid) << 64) |
        (static_cast<__uint128_t>(tag.iova & iova_mask) << 20) |
        static_cast<__uint128_t>(tag.pscid & pscid_mask);
    __uint128_t temp1 = array ^ (array >> 40);
    __uint128_t temp2 = temp1 ^ (temp1 >> 20);

    return static_cast<uint32_t>(temp2) & mask;
}

iova_t PTCache::align_iova(iova_t iova, PageSize ps) {
    switch (ps) {
        case PageSize::PAGE_4K:   return iova & ~static_cast<iova_t>(0xFFF);  // 清低12bit，页对齐
        case PageSize::PAGE_2M:   return iova & ~static_cast<iova_t>(0x1FFFFF);  // 清低21bit
        case PageSize::PAGE_1G:   return iova & ~static_cast<iova_t>(0x3FFFFFFF);  // 清低30bit
        case PageSize::PAGE_512G: return iova & ~static_cast<iova_t>(0x7FFFFFFFFF);  // 清低39bit
        default: return iova & ~static_cast<iova_t>(0xFFF);  // 默认4K对齐
    }
}

} // namespace iommu
