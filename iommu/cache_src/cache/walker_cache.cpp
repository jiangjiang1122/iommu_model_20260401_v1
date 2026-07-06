#include "cache/walker_cache.h"

#include "sysc/kernel/sc_dynamic_processes.h"

namespace iommu {

namespace {

iova_t extract_bits(iova_t value, uint8_t hi, uint8_t lo) {
    const uint8_t width = hi - lo + 1;
    const iova_t mask = (static_cast<iova_t>(1) << width) - 1;
    return (value >> lo) & mask;
}

} // namespace

// ============================================================
// WalkerSubCache 实现
// ============================================================

WalkerSubCache::WalkerSubCache(sc_module_name name, const CacheConfig& cfg,
                               StatsCollector& stats, const std::string& cache_name,
                               uint8_t level)
    : CacheBase<WalkerTag, WalkerData>(name, cfg, stats, cache_name),
      level_(level)
{
}

uint32_t WalkerSubCache::hash_function(const WalkerTag& tag) const {
    // LSB 低位有效策略: va_segment[5:0] XOR gscid[5:0] XOR pscid[5:0]
    uint32_t mask = num_sets_ - 1;
    uint32_t va_low = static_cast<uint32_t>(tag.va_segment) & mask;
    uint32_t gscid_low = static_cast<uint32_t>(tag.gscid) & mask;
    uint32_t pscid_low = tag.pscid & mask;
    return (va_low ^ gscid_low ^ pscid_low) & mask;
}

bool WalkerSubCache::lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                            bool va_pa_flag, bool stage_flag,
                            bool sv48_flag, bool x4_mode_flag,
                            WalkerData& out_data, sc_time& latency) {
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.va_segment = WalkerCache::extract_addr_segment(
        va, level_, va_pa_flag, sv48_flag, x4_mode_flag);
    tag.level = level_;
    tag.va_pa_flag = va_pa_flag;
    tag.stage_flag = stage_flag;
    tag.sv48_flag = sv48_flag;
    tag.x4_mode_flag = x4_mode_flag;
    tag.is_s2 = false;
    return CacheBase<WalkerTag, WalkerData>::lookup(tag, out_data, latency);
}

// [S2] S2 Cache子表查询: tag构造时设is_s2=true, va_pa_flag=false(GPA)
bool WalkerSubCache::lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                               bool sv48_flag, bool x4_mode_flag,
                               WalkerData& out_data, sc_time& latency) {
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    // S2 Cache: GPA作为key，addr_is_va=false
    tag.va_segment = WalkerCache::extract_addr_segment(
        gpa, level_, false, sv48_flag, x4_mode_flag);
    tag.level = level_;
    tag.va_pa_flag = false;   // GPA不是VA
    tag.stage_flag = true;    // 两阶段
    tag.sv48_flag = sv48_flag;
    tag.x4_mode_flag = x4_mode_flag;
    tag.is_s2 = true;         // [S2] 标识S2 Cache查询
    return CacheBase<WalkerTag, WalkerData>::lookup(tag, out_data, latency);
}

WalkerSubCache::UpdateResult
WalkerSubCache::update_entry(const WalkerTag& tag, const WalkerData& data,
                             bool direct_write) {
    UpdateResult result;
    
    // 关键修复：跳过无效数据的缓存写入，防止缓存污染
    // 如果WalkerData.valid=0，说明PTW没有缓存该层级的中间结果
    // 不应将(next_ppn=0x0, valid=0)写入缓存，否则后续LOOKUP会错误HIT
    if (!data.reserved.valid) {
        printf("[t=%llu ns][WALKER_CACHE] SKIP UPDATE: %s, gscid=%u, pscid=%u, level=%d, va_segment=0x%lx (valid=0)\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               cache_name_.c_str(), tag.gscid, tag.pscid, tag.level, tag.va_segment);
        fflush(stdout);
        result.updated = false;
        result.latency = SC_ZERO_TIME;
        return result;
    }
    
    arbitrate_ram_access(CacheOpType::FILL, [&]() {
        const uint32_t set = hash_function(tag);
        assert(set < num_sets_);

        if (direct_write) {
            const uint32_t way = 0;
            if (cache_array_[set][way].valid && !(cache_array_[set][way].tag == tag)) {
                stats_.record_eviction(cache_name_);
            }
            cache_array_[set][way].fill(tag, data, false);
            if (replacement_) replacement_->access(set, way);
            const sc_time access_latency =
                cycles_to_time(cfg_.hash_latency_cycles +
                               cfg_.write_way_latency_cycles);
            result.latency = execution_latency_with_arbiter(access_latency);
            stats_.record_latency(cache_name_, result.latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            result.updated = true;
            return;
        }

        const int existing_way = find_way(set, tag);
        if (existing_way >= 0) {
            cache_array_[set][existing_way].data = data;
            cache_array_[set][existing_way].valid = true;
            cache_array_[set][existing_way].from_prefetch = false;
            cache_array_[set][existing_way].access_count = 0;
            if (replacement_) replacement_->access(set, static_cast<uint32_t>(existing_way));
            const sc_time access_latency = fill_hit_access_latency();
            result.latency = execution_latency_with_arbiter(access_latency);
            stats_.record_latency(cache_name_, result.latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            result.updated = true;
            return;
        }

        const int empty_way = find_empty_way(set);
        if (empty_way >= 0) {
            cache_array_[set][empty_way].fill(tag, data, false);
            if (replacement_) replacement_->access(set, static_cast<uint32_t>(empty_way));
            const sc_time access_latency = fill_invalid_way_access_latency();
            result.latency = execution_latency_with_arbiter(access_latency);
            stats_.record_latency(cache_name_, result.latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            result.updated = true;
            return;
        }

        uint32_t victim_way = 0;
        if (replacement_) {
            victim_way = replacement_->find_victim(set);
        }
        if (cache_array_[set][victim_way].valid) {
            stats_.record_eviction(cache_name_);
        }
        cache_array_[set][victim_way].fill(tag, data, false);
        if (replacement_) {
            auto* srrip = dynamic_cast<SRRIPPolicy*>(replacement_.get());
            if (srrip) {
                srrip->on_insert(set, victim_way);
            } else {
                replacement_->access(set, victim_way);
            }
        }
        const sc_time access_latency = fill_replacement_access_latency();
        result.latency = execution_latency_with_arbiter(access_latency);
        stats_.record_latency(cache_name_, result.latency.to_seconds() * 1e9);
        consume_delay(access_latency);
        result.updated = true;
    });
    return result;
}

uint32_t WalkerSubCache::invalidate_vma(gscid_t gscid, pscid_t pscid,
                                        iova_t iova, bool has_gscid,
                                        bool has_pscid, bool has_iova,
                                        CacheInvalidateMode mode,
                                        sc_time* latency) {
    auto predicate = [=](const WalkerTag& tag, const WalkerData&) {
        if (has_gscid && tag.gscid != gscid) return false;
        if (has_pscid && tag.pscid != pscid) return false;
        if (has_iova && tag.va_segment != WalkerCache::extract_addr_segment(
                            iova, tag.level, tag.va_pa_flag, tag.sv48_flag,
                            tag.x4_mode_flag)) {
            return false;
        }
        return true;
    };

    if (mode == CacheInvalidateMode::GLOBAL) {
        return invalidate_all_entries(latency);
    }

    if (mode == CacheInvalidateMode::PRECISE && has_iova && has_gscid && has_pscid) {
        uint32_t affected = 0;
        std::vector<uint32_t> visited_sets;

        for (bool addr_is_va : {false, true}) {
            for (bool sv48 : {false, true}) {
                if (!sv48 && level_ == 1) continue;
                for (bool x4_mode : {false, true}) {
                    WalkerTag hash_tag;
                    hash_tag.gscid = gscid;
                    hash_tag.pscid = pscid;
                    hash_tag.level = level_;
                    hash_tag.va_segment = WalkerCache::extract_addr_segment(
                        iova, level_, addr_is_va, sv48, x4_mode);
                    hash_tag.va_pa_flag = addr_is_va;
                    hash_tag.sv48_flag = sv48;
                    hash_tag.x4_mode_flag = x4_mode;

                    const uint32_t set = hash_function(hash_tag);
                    bool visited = false;
                    for (uint32_t visited_set : visited_sets) {
                        if (visited_set == set) {
                            visited = true;
                            break;
                        }
                    }
                    if (visited) continue;
                    visited_sets.push_back(set);

                    affected += invalidate_precise_by_line(
                        hash_tag,
                        predicate,
                        [](const WalkerTag&, const WalkerData&) {},
                        latency);
                }
            }
        }
        return affected;
    }

    return invalidate_scan_by_line(
        predicate,
        [](const WalkerTag&, const WalkerData&) {},
        latency);
}

uint32_t WalkerSubCache::invalidate_by_gscid(gscid_t gscid, sc_time* latency) {
    return invalidate_scan_by_line(
        [gscid](const WalkerTag& tag, const WalkerData&) {
            return tag.gscid == gscid;
        },
        [](const WalkerTag&, const WalkerData&) {},
        latency);
}

uint32_t WalkerSubCache::invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                                   sc_time* latency) {
    return invalidate_scan_by_line(
        [gscid, pscid](const WalkerTag& tag, const WalkerData&) {
            return tag.gscid == gscid && tag.pscid == pscid;
        },
        [](const WalkerTag&, const WalkerData&) {},
        latency);
}

uint32_t WalkerSubCache::invalidate_global(sc_time* latency) {
    return invalidate_all_entries(latency);
}

// ============================================================
// WalkerCache 实现
// ============================================================

WalkerCache::WalkerCache(sc_module_name name,
                         const CacheConfig& cfg1,
                         const CacheConfig& cfg2,
                         const CacheConfig& cfg3,
                         StatsCollector& stats)
    : sc_module(name)
{
    ptw_c1_ = std::make_unique<WalkerSubCache>(
        "ptw_c1", cfg1, stats, "walker_ptw_c1", 1);
    ptw_c2_ = std::make_unique<WalkerSubCache>(
        "ptw_c2", cfg2, stats, "walker_ptw_c2", 2);
    ptw_c3_ = std::make_unique<WalkerSubCache>(
        "ptw_c3", cfg3, stats, "walker_ptw_c3", 3);

    // 上电主动无效化所有条目（确保初始状态干净，避免残留数据污染）
    uint32_t cleared = 0;
    cleared += ptw_c1_->invalidate_global(nullptr);
    cleared += ptw_c2_->invalidate_global(nullptr);
    cleared += ptw_c3_->invalidate_global(nullptr);
    printf("[WALKER_CACHE] POWER-ON RESET: all entries invalidated (cleared=%u)\n", cleared);
    fflush(stdout);
}

WalkerCache::~WalkerCache() = default;

bool WalkerCache::lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                         bool va_pa_flag, bool stage_flag,
                         bool sv48_flag, bool x4_mode_flag,
                         WalkerData& out_data, uint8_t& hit_level,
                         sc_time& latency) {
    latency = SC_ZERO_TIME;
    hit_level = 0;

    // [串行全查] 查询所有三级 Cache，模拟并行查询的统计效果
    // 每级都查询，不提前退出，最后仲裁返回最高级命中结果
    
    WalkerData data_c3, data_c2, data_c1;
    sc_time lat_c3 = SC_ZERO_TIME, lat_c2 = SC_ZERO_TIME, lat_c1 = SC_ZERO_TIME;
    bool hit_c3 = false, hit_c2 = false, hit_c1 = false;
    
    // 查询 C3
    iova_t va_seg_c3 = extract_addr_segment(va, 3, va_pa_flag, sv48_flag, x4_mode_flag);
    hit_c3 = ptw_c3_->lookup(gscid, pscid, va, va_pa_flag, stage_flag,
                             sv48_flag, x4_mode_flag, data_c3, lat_c3);
    if (hit_c3) {
        printf("[t=%llu ns][WALKER_CACHE] HIT: ptw_c3 (level=3), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu, next_ppn=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, va, (unsigned long)va_seg_c3, data_c3.next_ppn);
        fflush(stdout);
    } else {
        printf("[t=%llu ns][WALKER_CACHE] MISS: ptw_c3 (level=3), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, va, (unsigned long)va_seg_c3);
        fflush(stdout);
    }
    
    // 查询 C2
    iova_t va_seg_c2 = extract_addr_segment(va, 2, va_pa_flag, sv48_flag, x4_mode_flag);
    hit_c2 = ptw_c2_->lookup(gscid, pscid, va, va_pa_flag, stage_flag,
                             sv48_flag, x4_mode_flag, data_c2, lat_c2);
    if (hit_c2) {
        printf("[t=%llu ns][WALKER_CACHE] HIT: ptw_c2 (level=2), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu, next_ppn=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, va, (unsigned long)va_seg_c2, data_c2.next_ppn);
        fflush(stdout);
    } else {
        printf("[t=%llu ns][WALKER_CACHE] MISS: ptw_c2 (level=2), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, va, (unsigned long)va_seg_c2);
        fflush(stdout);
    }
    
    // 查询 C1（仅 Sv48 模式）
    if (!sv39_mode_ && sv48_flag) {
        iova_t va_seg_c1 = extract_addr_segment(va, 1, va_pa_flag, sv48_flag, x4_mode_flag);
        hit_c1 = ptw_c1_->lookup(gscid, pscid, va, va_pa_flag, stage_flag,
                                 sv48_flag, x4_mode_flag, data_c1, lat_c1);
        if (hit_c1) {
            printf("[t=%llu ns][WALKER_CACHE] HIT: ptw_c1 (level=1), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu, next_ppn=0x%lx\n",
                   (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                   gscid, pscid, va, (unsigned long)va_seg_c1, data_c1.next_ppn);
            fflush(stdout);
        } else {
            printf("[t=%llu ns][WALKER_CACHE] MISS: ptw_c1 (level=1), gscid=%u, pscid=%u, iova=0x%lx, va_seg=%lu\n",
                   (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                   gscid, pscid, va, (unsigned long)va_seg_c1);
            fflush(stdout);
        }
    }
    
    // [仲裁] 按 C3 > C2 > C1 优先级选择最高级命中
    if (hit_c3) {
        out_data = data_c3;
        hit_level = 3;
        latency = lat_c3;  // 只计最高级的延时
    } else if (hit_c2) {
        out_data = data_c2;
        hit_level = 2;
        latency = lat_c2;
    } else if (hit_c1) {
        out_data = data_c1;
        hit_level = 1;
        latency = lat_c1;
    } else {
        // 全未命中：延时累加
        latency = lat_c3 + lat_c2 + lat_c1;
    }
    
    return (hit_c3 || hit_c2 || hit_c1);
}

// [S2] S2 Cache查询: 用GPA查询is_s2=1的cacheline
bool WalkerCache::lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                            bool sv48_flag, bool x4_mode_flag,
                            WalkerData& out_data, uint8_t& hit_level,
                            sc_time& latency) {
    latency = SC_ZERO_TIME;
    hit_level = 0;
    s2_lookup_count_++;

    // 与lookup()逻辑一致，但使用lookup_s2子表接口
    WalkerData data_c3, data_c2, data_c1;
    sc_time lat_c3 = SC_ZERO_TIME, lat_c2 = SC_ZERO_TIME, lat_c1 = SC_ZERO_TIME;
    bool hit_c3 = false, hit_c2 = false, hit_c1 = false;

    // 查询 S2_C3
    hit_c3 = ptw_c3_->lookup_s2(gscid, pscid, gpa, sv48_flag, x4_mode_flag, data_c3, lat_c3);
    if (hit_c3) {
        printf("[t=%llu ns][WALKER_S2_CACHE] HIT: ptw_c3 (level=3), gscid=%u, pscid=%u, gpa=0x%lx, next_ppn=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, gpa, data_c3.next_ppn);
        fflush(stdout);
    }

    // 查询 S2_C2
    hit_c2 = ptw_c2_->lookup_s2(gscid, pscid, gpa, sv48_flag, x4_mode_flag, data_c2, lat_c2);
    if (hit_c2) {
        printf("[t=%llu ns][WALKER_S2_CACHE] HIT: ptw_c2 (level=2), gscid=%u, pscid=%u, gpa=0x%lx, next_ppn=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, gpa, data_c2.next_ppn);
        fflush(stdout);
    }

    // 查询 S2_C1（仅Sv48x4模式）
    if (!sv39_mode_ && sv48_flag) {
        hit_c1 = ptw_c1_->lookup_s2(gscid, pscid, gpa, sv48_flag, x4_mode_flag, data_c1, lat_c1);
        if (hit_c1) {
            printf("[t=%llu ns][WALKER_S2_CACHE] HIT: ptw_c1 (level=1), gscid=%u, pscid=%u, gpa=0x%lx, next_ppn=0x%lx\n",
                   (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                   gscid, pscid, gpa, data_c1.next_ppn);
            fflush(stdout);
        }
    }

    // [仲裁] 按 C3 > C2 > C1 优先级选择最高级命中
    if (hit_c3) {
        out_data = data_c3;
        hit_level = 3;
        latency = lat_c3;
        s2_hit_c3_count_++;
    } else if (hit_c2) {
        out_data = data_c2;
        hit_level = 2;
        latency = lat_c2;
        s2_hit_c2_count_++;
    } else if (hit_c1) {
        out_data = data_c1;
        hit_level = 1;
        latency = lat_c1;
        s2_hit_c1_count_++;
    } else {
        latency = lat_c3 + lat_c2 + lat_c1;
        s2_miss_count_++;
        printf("[t=%llu ns][WALKER_S2_CACHE] MISS: all levels, gscid=%u, pscid=%u, gpa=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, gpa);
        fflush(stdout);
    }

    return (hit_c3 || hit_c2 || hit_c1);
}

void WalkerCache::fill(gscid_t gscid, pscid_t pscid, iova_t va,
                       uint8_t level, const WalkerData& data) {
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.level = level;
    tag.va_pa_flag = walker_va_pa_flag(data);
    tag.stage_flag = walker_stage_flag(data);
    tag.sv48_flag = walker_sv48_flag(data);
    tag.x4_mode_flag = walker_x4_mode_flag(data);
    tag.va_segment = extract_addr_segment(
        va, level, tag.va_pa_flag, tag.sv48_flag, tag.x4_mode_flag);

    switch (level) {
        case 1:
            if (!sv39_mode_ && tag.sv48_flag) ptw_c1_->fill(tag, data);
            break;
        case 2:
            ptw_c2_->fill(tag, data);
            break;
        case 3:
            ptw_c3_->fill(tag, data);
            break;
    }
}

WalkerCache::UpdateResult WalkerCache::update(gscid_t gscid, pscid_t pscid,
                                              iova_t va, WalkerUpdateKind kind,
                                              const WalkerData& ptwc1_data,
                                              const WalkerData& ptwc2_data,
                                              const WalkerData& ptwc3_data) {
    iova_t upd_va_seg_1 = extract_addr_segment(va, 1, true, true, false);
    iova_t upd_va_seg_2 = extract_addr_segment(va, 2, true, true, false);
    iova_t upd_va_seg_3 = extract_addr_segment(va, 3, true, true, false);
    printf("[t=%llu ns][WALKER_CACHE] UPDATE: gscid=%u, pscid=%u, iova=0x%lx, kind=%d, ptwc1_valid=%d, ptwc2_valid=%d, ptwc3_valid=%d, va_seg[L1=%lu,L2=%lu,L3=%lu]\n",
           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
           gscid, pscid, va, static_cast<int>(kind),
           ptwc1_data.reserved.valid, ptwc2_data.reserved.valid, ptwc3_data.reserved.valid,
           (unsigned long)upd_va_seg_1, (unsigned long)upd_va_seg_2, (unsigned long)upd_va_seg_3);
    fflush(stdout);
    auto make_tag = [=](uint8_t level, const WalkerData& data) {
        WalkerTag tag;
        tag.gscid = gscid;
        tag.pscid = pscid;
        tag.level = level;
        tag.va_pa_flag = walker_va_pa_flag(data);
        tag.stage_flag = walker_stage_flag(data);
        tag.sv48_flag = walker_sv48_flag(data);
        tag.x4_mode_flag = walker_x4_mode_flag(data);
        tag.va_segment = extract_addr_segment(
            va, level, tag.va_pa_flag, tag.sv48_flag, tag.x4_mode_flag);
        return tag;
    };

    UpdateResult result;
    
    // 处理 NONE 类型：不需要更新，直接返回
    if (kind == WalkerUpdateKind::NONE) {
        update_none_count_++;  // 统计跳过次数
        printf("[t=%llu ns][WALKER_CACHE] UPDATE: NONE (skip), gscid=%u, pscid=%u, iova=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, va);
        fflush(stdout);
        return result;
    }
    
    const bool update_ptwc1 = kind == WalkerUpdateKind::PTWC_1_2_3;
    const bool update_ptwc2 = kind == WalkerUpdateKind::PTWC_1_2_3 ||
                              kind == WalkerUpdateKind::PTWC_2_3;

    // 统计更新类型
    if (kind == WalkerUpdateKind::PTWC_1_2_3) {
        update_ptwc_123_count_++;
    } else if (kind == WalkerUpdateKind::PTWC_2_3) {
        update_ptwc_23_count_++;
    } else if (kind == WalkerUpdateKind::PTWC_3) {
        update_ptwc_3_count_++;
    }

    auto merge_result = [&result](bool& updated_flag,
                                  const WalkerSubCache::UpdateResult& partial) {
        updated_flag = partial.updated;
        if (partial.latency > result.latency) result.latency = partial.latency;
    };

    if (!sc_get_current_process_handle().valid()) {
        if (update_ptwc1 && !sv39_mode_ && walker_sv48_flag(ptwc1_data)) {
            auto r1 = ptw_c1_->update_entry(make_tag(1, ptwc1_data), ptwc1_data,
                                            true);
            merge_result(result.updated_ptwc1, r1);
        }

        if (update_ptwc2) {
            auto r2 = ptw_c2_->update_entry(make_tag(2, ptwc2_data), ptwc2_data,
                                            false);
            merge_result(result.updated_ptwc2, r2);
        }

        auto r3 = ptw_c3_->update_entry(make_tag(3, ptwc3_data), ptwc3_data,
                                        false);
        merge_result(result.updated_ptwc3, r3);
        return result;
    }

    sc_core::sc_join join;
    auto spawn_update = [&](const char* name,
                            WalkerSubCache* cache,
                            WalkerTag tag,
                            WalkerData data,
                            bool direct_write,
                            bool& updated_flag) {
        bool* updated = &updated_flag;
        auto handle = sc_core::sc_spawn(
            [&, cache, tag, data, direct_write, updated]() {
                auto partial = cache->update_entry(tag, data, direct_write);
                merge_result(*updated, partial);
            },
            sc_core::sc_gen_unique_name(name));
        join.add_process(handle);
    };

    if (update_ptwc1 && !sv39_mode_ && walker_sv48_flag(ptwc1_data)) {
        spawn_update("walker_update_ptwc1", ptw_c1_.get(),
                     make_tag(1, ptwc1_data), ptwc1_data, true,
                     result.updated_ptwc1);
    }

    if (update_ptwc2) {
        spawn_update("walker_update_ptwc2", ptw_c2_.get(),
                     make_tag(2, ptwc2_data), ptwc2_data, false,
                     result.updated_ptwc2);
    }

    spawn_update("walker_update_ptwc3", ptw_c3_.get(),
                 make_tag(3, ptwc3_data), ptwc3_data, false,
                 result.updated_ptwc3);

    join.wait();
    return result;
}

// [S2] S2 Cache更新: 与update()逻辑一致，但构造tag时设is_s2=true, va_pa_flag=false
WalkerCache::UpdateResult WalkerCache::update_s2(gscid_t gscid, pscid_t pscid,
                                                  iova_t gpa, WalkerUpdateKind kind,
                                                  const WalkerData& ptwc1_data,
                                                  const WalkerData& ptwc2_data,
                                                  const WalkerData& ptwc3_data) {
    printf("[t=%llu ns][WALKER_S2_CACHE] UPDATE: gscid=%u, pscid=%u, gpa=0x%lx, kind=%d, ptwc1_valid=%d, ptwc2_valid=%d, ptwc3_valid=%d\n",
           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
           gscid, pscid, gpa, static_cast<int>(kind),
           ptwc1_data.reserved.valid, ptwc2_data.reserved.valid, ptwc3_data.reserved.valid);
    fflush(stdout);

    // S2 tag构造: is_s2=true, va_pa_flag=false (GPA), stage_flag=true
    auto make_tag = [=](uint8_t level, const WalkerData& data) {
        WalkerTag tag;
        tag.gscid = gscid;
        tag.pscid = pscid;
        tag.level = level;
        tag.va_pa_flag = false;    // [S2] GPA不是VA
        tag.stage_flag = true;     // [S2] 两阶段
        tag.sv48_flag = walker_sv48_flag(data);
        tag.x4_mode_flag = walker_x4_mode_flag(data);
        tag.is_s2 = true;          // [S2]
        tag.va_segment = extract_addr_segment(
            gpa, level, false, tag.sv48_flag, tag.x4_mode_flag);
        return tag;
    };

    UpdateResult result;

    if (kind == WalkerUpdateKind::NONE) {
        printf("[t=%llu ns][WALKER_S2_CACHE] UPDATE: NONE (skip), gscid=%u, pscid=%u, gpa=0x%lx\n",
               (unsigned long long)sc_core::sc_time_stamp().value()/1000,
               gscid, pscid, gpa);
        fflush(stdout);
        return result;
    }

    const bool update_ptwc1 = kind == WalkerUpdateKind::PTWC_1_2_3;
    const bool update_ptwc2 = kind == WalkerUpdateKind::PTWC_1_2_3 ||
                              kind == WalkerUpdateKind::PTWC_2_3;

    auto merge_result = [&result](bool& updated_flag,
                                  const WalkerSubCache::UpdateResult& partial) {
        updated_flag = partial.updated;
        if (partial.latency > result.latency) result.latency = partial.latency;
    };

    // 串行更新（S2 Cache使用简单路径，不需要并行spawn）
    if (update_ptwc1 && !sv39_mode_ && walker_sv48_flag(ptwc1_data)) {
        auto r1 = ptw_c1_->update_entry(make_tag(1, ptwc1_data), ptwc1_data, true);
        merge_result(result.updated_ptwc1, r1);
    }

    if (update_ptwc2) {
        auto r2 = ptw_c2_->update_entry(make_tag(2, ptwc2_data), ptwc2_data, false);
        merge_result(result.updated_ptwc2, r2);
    }

    auto r3 = ptw_c3_->update_entry(make_tag(3, ptwc3_data), ptwc3_data, false);
    merge_result(result.updated_ptwc3, r3);

    printf("[t=%llu ns][WALKER_S2_CACHE] UPDATE done: c1=%d, c2=%d, c3=%d\n",
           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
           result.updated_ptwc1, result.updated_ptwc2, result.updated_ptwc3);
    fflush(stdout);

    return result;
}

uint32_t WalkerCache::invalidate_by_gscid(gscid_t gscid, sc_time* latency) {
    uint32_t affected = 0;
    affected += ptw_c1_->invalidate_by_gscid(gscid, latency);
    affected += ptw_c2_->invalidate_by_gscid(gscid, latency);
    affected += ptw_c3_->invalidate_by_gscid(gscid, latency);
    return affected;
}

uint32_t WalkerCache::invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                                sc_time* latency) {
    uint32_t affected = 0;
    affected += ptw_c1_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    affected += ptw_c2_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    affected += ptw_c3_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    return affected;
}

uint32_t WalkerCache::invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                                     bool has_gscid, bool has_pscid, bool has_iova,
                                     CacheInvalidateMode mode,
                                     sc_time* latency) {
    uint32_t affected = 0;
    affected += ptw_c1_->invalidate_vma(gscid, pscid, iova, has_gscid,
                                        has_pscid, has_iova, mode, latency);
    affected += ptw_c2_->invalidate_vma(gscid, pscid, iova, has_gscid,
                                        has_pscid, has_iova, mode, latency);
    affected += ptw_c3_->invalidate_vma(gscid, pscid, iova, has_gscid,
                                        has_pscid, has_iova, mode, latency);
    return affected;
}

uint32_t WalkerCache::invalidate_gvma(gscid_t gscid, bool has_gscid,
                                      CacheInvalidateMode mode,
                                      sc_time* latency) {
    if (mode == CacheInvalidateMode::GLOBAL) {
        return invalidate_global(latency);
    }
    if (has_gscid) {
        return invalidate_by_gscid(gscid, latency);
    }
    return invalidate_global(latency);
}

uint32_t WalkerCache::invalidate_global(sc_time* latency) {
    uint32_t affected = 0;
    affected += ptw_c1_->invalidate_global(latency);
    affected += ptw_c2_->invalidate_global(latency);
    affected += ptw_c3_->invalidate_global(latency);
    return affected;
}

void WalkerCache::set_clock_period(const sc_time& clk) {
    ptw_c1_->set_clock_period(clk);
    ptw_c2_->set_clock_period(clk);
    ptw_c3_->set_clock_period(clk);
}

iova_t WalkerCache::extract_addr_segment(iova_t addr, uint8_t level,
                                         bool addr_is_va, bool sv48,
                                         bool x4_mode) {
    const bool use_x4 = addr_is_va && x4_mode;

    if (sv48) {
        const iova_t root = addr_is_va
            ? extract_bits(addr, use_x4 ? 49 : 47, 39)
            : extract_bits(addr, 55, 39);
        if (level == 1) return root;

        const iova_t root_l2 = (root << 9) | extract_bits(addr, 38, 30);
        if (level == 2) return root_l2;
        if (level == 3) return (root_l2 << 9) | extract_bits(addr, 29, 21);
        return addr;
    }

    // Sv39 has no PTWc_1 entry. PTWc_2 starts at VPN/PPN[2].
    if (level == 1) return 0;

    const iova_t root = addr_is_va
        ? extract_bits(addr, use_x4 ? 40 : 38, 30)
        : extract_bits(addr, 55, 30);
    if (level == 2) return root;
    if (level == 3) return (root << 9) | extract_bits(addr, 29, 21);
    return addr;
}

} // namespace iommu
