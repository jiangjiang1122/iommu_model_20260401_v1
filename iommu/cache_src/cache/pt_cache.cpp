#include "cache/pt_cache.h"
#include <cassert>

namespace iommu {

PTCache::PTCache(sc_module_name name, const CacheConfig& cfg,
                 StatsCollector& stats)
    : CacheBase<PTTag, PTData>(name, cfg, stats, "pt_cache")
{
    // [多RAM] 初始化 RAM 分组参数: num_rams 必须为2的幂且整除 num_sets
    num_rams_ = (cfg.num_rams == 0) ? 1 : cfg.num_rams;
    assert((num_rams_ & (num_rams_ - 1)) == 0 && "num_rams must be power of 2");
    assert(num_sets_ % num_rams_ == 0 && "num_rams must divide num_sets");
    sets_per_ram_ = num_sets_ / num_rams_;
    log2_num_rams_ = 0;
    for (uint32_t v = num_rams_; v > 1; v >>= 1) log2_num_rams_++;
    printf("[PT_CACHE] Multi-RAM config: num_rams=%u, sets_per_ram=%u, ram_fifo_depth=%u\n",
           num_rams_, sets_per_ram_, cfg.ram_fifo_depth);
}

// [多RAM] 构造 lookup/fill 共用 tag
PTTag PTCache::make_tag(gscid_t gscid, pscid_t pscid, iova_t iova,
                        TransStage stage, bool sv48, bool gstage_x4) {
    PTTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.iova = align_iova(iova, PageSize::PAGE_4K);
    tag.stage = stage;
    tag.sv48 = sv48;
    tag.gstage_x4 = gstage_x4;
    return tag;
}

bool PTCache::lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                        TransStage stage, bool sv48, bool gstage_x4,
                        PTData& out_data, sc_time& latency) {
    PTTag tag = make_tag(gscid, pscid, iova, stage, sv48, gstage_x4);
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
    // [多RAM] 新映射: 低 log2(num_rams) 位选 RAM, 高位作 RAM 内索引
    // global_set = ram_id * sets_per_ram + set_in_ram, RAM i 独占连续 set 区间
    // num_rams=1 时退化为原 raw & (num_sets-1)
    uint32_t raw = raw_hash(tag);
    if (num_rams_ <= 1) {
        return raw & (num_sets_ - 1);
    }
    uint32_t ram_id = raw & (num_rams_ - 1);
    uint32_t set_in_ram = (raw >> log2_num_rams_) & (sets_per_ram_ - 1);
    return ram_id * sets_per_ram_ + set_in_ram;
}

// [多RAM] 未掩码原始hash(原 hash_function 去掉 & mask)
uint32_t PTCache::raw_hash(const PTTag& tag) const {
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 32) - 1;  // 32-bit page number
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    // [FIX] IOVA右移12位(去掉4KB页内偏移), 避免页对齐IOVA全部映射到 Set 0
    __uint128_t array =
        (static_cast<__uint128_t>(tag.gscid) << 64) |
        (static_cast<__uint128_t>((tag.iova >> 12) & iova_mask) << 20) |
        static_cast<__uint128_t>(tag.pscid & pscid_mask);
    __uint128_t temp1 = array ^ (array >> 40);
    __uint128_t temp2 = temp1 ^ (temp1 >> 20);

    return static_cast<uint32_t>(temp2);
}

// [多RAM] 计算任务所属 RAM 组号 (hash 仅依赖 gscid/pscid/iova页号)
uint32_t PTCache::compute_ram_id(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    if (num_rams_ <= 1) return 0;
    PTTag tag = make_tag(gscid, pscid, iova, TransStage::STAGE1_AND_2, true, false);
    return raw_hash(tag) & (num_rams_ - 1);
}

// [多RAM] RAM 原子段查询: 纯功能(无wait/无仲裁), 同 RAM 串行由单 worker 线程保证
// ram_latency = read_set + compare (不含hash, hash已由Hash单元消耗)
bool PTCache::lookup_pt_ram(gscid_t gscid, pscid_t pscid, iova_t iova,
                            TransStage stage, bool sv48, bool gstage_x4,
                            PTData& out_data, sc_time& ram_latency) {
    PTTag tag = make_tag(gscid, pscid, iova, stage, sv48, gstage_x4);
    uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    stats_.record_access(cache_name_);
    stats_.record_access_timestamp(cache_name_, sc_time_stamp().to_seconds() * 1e9);

    ram_latency = cycles_to_time(cfg_.read_set_latency_cycles +
                                 cfg_.compare_latency_cycles);
    // [STAT] 口径与单RAM版本一致: 记录含hash的完整操作延时
    const double op_latency_ns =
        (hash_stage_latency() + ram_latency).to_seconds() * 1e9;

    int way = find_way(set, tag);
    if (way >= 0) {
        // Hit
        out_data = cache_array_[set][way].data;
        if (replacement_) replacement_->access(set, way);
        if (cache_array_[set][way].from_prefetch) {
            stats_.record_prefetch_hit(cache_name_);
            cache_array_[set][way].from_prefetch = false;
        }
        cache_array_[set][way].access_count++;
        stats_.record_hit(cache_name_);
        stats_.record_lookup(cache_name_);
        stats_.record_phase_lookup(cache_name_, phase_tracker_);
        stats_.record_latency(cache_name_, op_latency_ns);
        return true;
    }

    // Miss
    stats_.record_miss(cache_name_);
    stats_.record_lookup(cache_name_);
    stats_.record_phase_lookup(cache_name_, phase_tracker_);
    stats_.record_latency(cache_name_, op_latency_ns);
    return false;
}

// [多RAM] RAM 原子段填充: 纯功能(无wait/无仲裁)
// ram_latency = read_set + fill_compute_index_* + write_way (不含hash)
void PTCache::fill_pt_ram(gscid_t gscid, pscid_t pscid, iova_t iova,
                          TransStage stage, const PTData& data, bool from_prefetch,
                          sc_time& ram_latency) {
    PTData stored_data = data;
    stored_data.reserved.trans_type = static_cast<uint32_t>(stage);
    stored_data.reserved.iova_is_va = (stage == TransStage::STAGE2_ONLY) ? 0U : 1U;

    PTTag tag = make_tag(gscid, pscid, iova, stage,
                         pt_sv48_mode(stored_data), pt_gstage_x4_mode(stored_data));
    uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    stats_.record_access_timestamp(cache_name_, sc_time_stamp().to_seconds() * 1e9);

    const uint32_t base_cycles = cfg_.read_set_latency_cycles +
                                 cfg_.write_way_latency_cycles;

    // 已存在: fill_hit
    int existing_way = find_way(set, tag);
    if (existing_way >= 0) {
        ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_hit_cycles);
        stats_.record_fill_hit(cache_name_);
        stats_.record_phase_fill_hit(cache_name_, phase_tracker_);
        cache_array_[set][existing_way].data = stored_data;
        cache_array_[set][existing_way].valid = true;
        cache_array_[set][existing_way].from_prefetch = from_prefetch;
        cache_array_[set][existing_way].access_count = 0;
        if (replacement_) replacement_->access(set, existing_way);
        if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
        stats_.record_latency(cache_name_,
            (hash_stage_latency() + ram_latency).to_seconds() * 1e9);
        return;
    }

    // 空闲 way: fill_invalid
    int empty_way = find_empty_way(set);
    if (empty_way >= 0) {
        ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_invalid_cycles);
        stats_.record_fill_invalid(cache_name_);
        stats_.record_phase_fill_invalid(cache_name_, phase_tracker_);
        cache_array_[set][empty_way].fill(tag, stored_data, from_prefetch);
        if (replacement_) replacement_->access(set, empty_way);
        if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
        stats_.record_latency(cache_name_,
            (hash_stage_latency() + ram_latency).to_seconds() * 1e9);
        return;
    }

    // 需替换: fill_replacement
    ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_replacement_cycles);
    stats_.record_fill_replace(cache_name_);
    stats_.record_phase_fill_replace(cache_name_, phase_tracker_);
    uint32_t victim_way = 0;
    if (replacement_) {
        victim_way = replacement_->find_victim(set);
    }
    if (cache_array_[set][victim_way].valid) {
        stats_.record_eviction(cache_name_);
    }
    cache_array_[set][victim_way].fill(tag, stored_data, from_prefetch);
    if (replacement_) {
        auto* srrip = dynamic_cast<SRRIPPolicy*>(replacement_.get());
        if (srrip) {
            srrip->on_insert(set, victim_way);
        } else {
            replacement_->access(set, victim_way);
        }
    }
    if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
    stats_.record_latency(cache_name_,
        (hash_stage_latency() + ram_latency).to_seconds() * 1e9);
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
