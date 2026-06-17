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
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 32) - 1;  // 32-bit page number
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    // [FIX] IOVA右移12位(去掉4KB页内偏移), 避免页对齐IOVA全部映射到Set 0
    __uint128_t array =
        (static_cast<__uint128_t>(tag.gscid) << 64) |
        (static_cast<__uint128_t>((tag.iova >> 12) & iova_mask) << 20) |
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

// =====================================================================
// NEW: PT Cache去重+预取接口实现（4KB页简化版）
// =====================================================================

bool PTCache::insert_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova,
                                 TransStage stage, bool sv48, bool gstage_x4,
                                 uint16_t head_index, bool is_req,
                                 sc_time* latency) {
    // 构造占位CL的tag
    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);  // 4KB页对齐
    tag.stage = stage;
    tag.sv48 = sv48;
    tag.gstage_x4 = gstage_x4;

    // [兜底逻辑] 检查Cache set是否所有way都是is_req=1的占位CL
    // 如果是，则无法插入，返回false
    uint32_t set = hash_function(tag);
    bool all_protected = true;
    bool has_invalid_way = false;
    
    for (uint32_t w = 0; w < num_ways_; w++) {
        if (!cache_array_[set][w].valid) {
            has_invalid_way = true;
            all_protected = false;
            break;
        }
        
        // 检查是否为常规CL或非保护的占位CL
        if (cache_array_[set][w].data.reserved.is_ph == 0) {
            // 常规CL，可以被替换
            all_protected = false;
            break;
        }
        
        if (cache_array_[set][w].data.reserved.is_ph == 1 && 
            cache_array_[set][w].data.reserved.is_req == 0) {
            // 预取占位CL (is_req=0)，可以被替换
            all_protected = false;
            break;
        }
        
        // is_ph=1 && is_req=1: 主任务占位CL，受保护
    }
    
    if (all_protected && !has_invalid_way) {
        // 所有way都是受保护的主任务占位CL，无法插入
        std::cout << "[PT_CACHE_FALLBACK] Set " << set << " full, all ways are is_req=1 placeholders. ";
        if (is_req) {
            std::cout << "Main task placeholder -> Fallback: direct PTW" << std::endl;
        } else {
            std::cout << "Prefetch placeholder -> Fallback: discard" << std::endl;
        }
        
        if (latency) {
            *latency = sc_time(1, SC_NS);
        }
        return false;  // 替换失败
    }

    // 构造占位CL的data
    PTData placeholder_data;
    placeholder_data.reserved.raw = 0;
    placeholder_data.reserved.valid = 1;
    placeholder_data.reserved.trans_type = static_cast<uint32_t>(stage);
    placeholder_data.reserved.input_page_size = 0;  // 4KB
    placeholder_data.reserved.result_page_size = 0;  // 4KB
    placeholder_data.reserved.iova_is_va = (stage == TransStage::STAGE2_ONLY) ? 0U : 1U;
    placeholder_data.reserved.sv48 = sv48 ? 1U : 0U;
    placeholder_data.reserved.gstage_x4 = gstage_x4 ? 1U : 0U;
    placeholder_data.reserved.is_ph = 1;  // 占位标志
    placeholder_data.reserved.head_index = head_index;  // Buffer链头索引
    // [V3.0] tail_index已移至Buffer entry，PT Cache不再存储
    placeholder_data.reserved.is_req = is_req ? 1U : 0U;  // 是否主任务

    // 调用基类fill插入（如果Cache满会触发替换）
    fill(tag, placeholder_data, false);

    if (latency) {
        *latency = sc_time(1, SC_NS);  // 占位CL插入延时
    }

    std::cout << "[PT_CACHE] insert_placeholder: iova=0x" << std::hex << tag.iova 
              << ", head_index=" << std::dec << static_cast<int>(head_index)
              << ", is_req=" << (is_req ? 1 : 0) << std::endl;

    return true;  // 插入成功
}

void PTCache::batch_update_placeholders(gscid_t gscid, pscid_t pscid,
                                        const std::vector<std::pair<iova_t, PTData>>& updates,
                                        TransStage stage, bool sv48, bool gstage_x4) {
    for (const auto& update : updates) {
        iova_t iova = update.first;
        PTData pt_data = update.second;

        // 先尝试查询是否已存在占位CL
        PTData existing_data;
        sc_time latency;
        bool hit = lookup_pt(gscid, pscid, iova, stage, sv48, gstage_x4, existing_data, latency);

        if (hit && existing_data.reserved.is_ph == 1) {
            // 命中占位CL：更新为常规CL
            PTTag tag;
            tag.gscid = gscid;
            tag.pscid = pscid;
            tag.iova = align_iova(iova, PageSize::PAGE_4K);
            tag.stage = stage;
            tag.sv48 = sv48;
            tag.gstage_x4 = gstage_x4;

            // 更新data：is_ph: 1->0，填充真实的PTE
            pt_data.reserved.is_ph = 0;
            pt_data.reserved.valid = 1;
            pt_data.reserved.trans_type = static_cast<uint32_t>(stage);
            pt_data.reserved.iova_is_va = (stage == TransStage::STAGE2_ONLY) ? 0U : 1U;
            pt_data.reserved.sv48 = sv48 ? 1U : 0U;
            pt_data.reserved.gstage_x4 = gstage_x4 ? 1U : 0U;

            // 直接fill更新cache line（会覆盖原有占位CL）
            fill(tag, pt_data, false);

            std::cout << "[PT_CACHE] batch_update: iova=0x" << std::hex << iova 
                      << " (placeholder -> regular)" << std::dec << std::endl;
        } else if (!hit) {
            // 未命中：新建常规CL
            fill_pt(gscid, pscid, iova, stage, pt_data, false);

            std::cout << "[PT_CACHE] batch_update: iova=0x" << std::hex << iova 
                      << " (new regular CL)" << std::dec << std::endl;
        } else {
            // 命中常规CL：直接更新PTE
            PTTag tag;
            tag.gscid = gscid;
            tag.pscid = pscid;
            tag.iova = align_iova(iova, PageSize::PAGE_4K);
            tag.stage = stage;
            tag.sv48 = sv48;
            tag.gstage_x4 = gstage_x4;

            // 直接fill更新cache line
            fill(tag, pt_data, false);

            std::cout << "[PT_CACHE] batch_update: iova=0x" << std::hex << iova 
                      << " (update existing regular CL)" << std::dec << std::endl;
        }
    }
}

bool PTCache::update_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova,
                                 TransStage stage, bool sv48, bool gstage_x4,
                                 uint16_t head_index, bool is_req) {
    // 构造tag
    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);
    tag.stage = stage;
    tag.sv48 = sv48;
    tag.gstage_x4 = gstage_x4;
    
    // 查找对应的占位CL
    PTData existing_data;
    sc_time latency;
    bool hit = lookup(tag, existing_data, latency);
    
    if (!hit) {
        std::cout << "[PT_CACHE] update_placeholder: iova=0x" << std::hex << tag.iova
                  << " -> MISS, cannot update" << std::dec << std::endl;
        return false;
    }
    
    if (existing_data.reserved.is_ph == 0) {
        std::cout << "[PT_CACHE] update_placeholder: iova=0x" << std::hex << tag.iova
                  << " -> HIT regular CL, cannot update" << std::dec << std::endl;
        return false;
    }
    
    // [V3.0] 更新占位CL的head_index和is_req（不再包含tail_index）
    existing_data.reserved.head_index = head_index;
    existing_data.reserved.is_req = is_req ? 1U : 0U;
    
    // 重新fill更新cache line
    fill(tag, existing_data, false);
    
    std::cout << "[PT_CACHE] update_placeholder: iova=0x" << std::hex << tag.iova
              << ", head=" << std::dec << static_cast<int>(head_index)
              << ", is_req=" << (is_req ? 1 : 0) << std::endl;
    
    return true;
}

// =============================================================
// [OPT] 直接更新占位CL，跳过内部冗余lookup
// 调用方已通过lookup_pt获取data，无需再次lookup
// =============================================================
void PTCache::update_placeholder_with_data(gscid_t gscid, pscid_t pscid, iova_t iova,
                                           TransStage stage, bool sv48, bool gstage_x4,
                                           PTData data, uint16_t head_index, bool is_req) {
    // 构造tag
    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);
    tag.stage = stage;
    tag.sv48 = sv48;
    tag.gstage_x4 = gstage_x4;

    // [OPT] 直接使用调用方提供的data，跳过lookup
    // 调用方已验证 is_ph=1, is_req=0
    data.reserved.head_index = head_index;
    data.reserved.is_req = is_req ? 1U : 0U;

    // fill更新cache line（FillHit路径，因为entry已存在）
    fill(tag, data, false);

    std::cout << "[PT_CACHE] update_placeholder_with_data: iova=0x" << std::hex << tag.iova
              << ", head=" << std::dec << static_cast<int>(head_index)
              << ", is_req=" << (is_req ? 1 : 0)
              << " [OPT: skip redundant lookup]" << std::endl;
}

} // namespace iommu
