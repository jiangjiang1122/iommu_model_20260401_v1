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
    init_s2_cache_array();
}

// [S2] 初始化独立的S2 Cache阵列
void WalkerSubCache::init_s2_cache_array() {
    s2_cache_array_.resize(num_sets_,
        std::vector<CacheLine<WalkerTag, WalkerData>>(num_ways_));
}

uint32_t WalkerSubCache::hash_function(const WalkerTag& tag) const {
    return set_from_raw(raw_hash(tag));
}

// [多RAM] raw hash -> 全局 set 号
// 低 log2(num_rams) 位选 RAM, 高位作 RAM 内索引;
// global_set = ram_id * sets_per_ram + set_in_ram; num_rams=1 时退化原逻辑
uint32_t WalkerSubCache::set_from_raw(uint32_t raw) const {
    if (num_rams_ <= 1) {
        return raw & (num_sets_ - 1);
    }
    uint32_t ram_id = raw & (num_rams_ - 1);
    uint32_t set_in_ram = (raw >> log2_num_rams_) & (sets_per_ram_ - 1);
    return ram_id * sets_per_ram_ + set_in_ram;
}

// [失效] 新哈希分量 (与 PT Cache 同构): temp1 仅由 gscid/pscid 得到(3bit),
// 使得指定 addr 的小范围失效可枚举 8 个 temp1 覆盖全部可能的 set。
uint32_t WalkerSubCache::hash_temp1(gscid_t gscid, pscid_t pscid) {
    return (static_cast<uint32_t>(gscid) ^ pscid) & 0x7U;
}

uint32_t WalkerSubCache::hash_temp2(iova_t va_segment) {
    return static_cast<uint32_t>(va_segment ^ (va_segment >> 22));
}

uint32_t WalkerSubCache::hash_combine(uint32_t temp1, uint32_t temp2) const {
    const uint32_t shift = (log2_num_sets_ > 3) ? (log2_num_sets_ - 3) : 0;
    return ((temp1 << shift) ^ temp2) & (num_sets_ - 1);
}

// [多RAM] 未掩码原始hash
uint32_t WalkerSubCache::raw_hash(const WalkerTag& tag) const {
    if (hash_v2_) {
        return hash_combine(hash_temp1(tag.gscid, tag.pscid),
                            hash_temp2(tag.va_segment));
    }
    // legacy: va_segment[5:0] XOR gscid[5:0] XOR pscid[5:0]
    uint32_t va_low = static_cast<uint32_t>(tag.va_segment);
    uint32_t gscid_low = static_cast<uint32_t>(tag.gscid);
    uint32_t pscid_low = tag.pscid;
    return va_low ^ gscid_low ^ pscid_low;
}

// [多RAM] 初始化RAM分组参数: num_rams必须为2的幂且整除num_sets
void WalkerSubCache::configure_multi_ram(uint32_t num_rams) {
    num_rams_ = (num_rams == 0) ? 1 : num_rams;
    assert((num_rams_ & (num_rams_ - 1)) == 0 && "walker num_rams must be power of 2");
    assert(num_sets_ % num_rams_ == 0 && "walker num_rams must divide num_sets");
    sets_per_ram_ = num_sets_ / num_rams_;
    log2_num_rams_ = 0;
    for (uint32_t v = num_rams_; v > 1; v >>= 1) log2_num_rams_++;
    // [失效] 新哈希需 log2(num_sets) 作为 temp1 移位量
    assert((num_sets_ & (num_sets_ - 1)) == 0 && "walker num_sets must be power of 2");
    log2_num_sets_ = 0;
    for (uint32_t v = num_sets_; v > 1; v >>= 1) log2_num_sets_++;
    hash_v2_ = (cfg_.hash_mode != "legacy");
    printf("[WALKER_CACHE] %s Multi-RAM config: num_rams=%u, sets_per_ram=%u, hash_mode=%s\n",
           cache_name_.c_str(), num_rams_, sets_per_ram_,
           hash_v2_ ? "inval_v2" : "legacy");
}

uint32_t WalkerSubCache::compute_ram_id(const WalkerTag& tag) const {
    if (num_rams_ <= 1) return 0;
    return raw_hash(tag) & (num_rams_ - 1);
}

// [多RAM] 原子段查询: 纯功能(无wait/无仲裁), 同RAM串行由单worker线程保证
// ram_latency = read_set + compare (不含hash, hash已由Hash线程消耗)
bool WalkerSubCache::lookup_ram(const WalkerTag& tag, WalkerData& out_data,
                                sc_time& ram_latency) {
    uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    stats_.record_access(cache_name_);
    ram_latency = cycles_to_time(cfg_.read_set_latency_cycles +
                                 cfg_.compare_latency_cycles);
    // [STAT] 口径与旧版本一致: 记录含hash的完整操作延时
    const double op_latency_ns =
        (cycles_to_time(cfg_.hash_latency_cycles) + ram_latency).to_seconds() * 1e9;

    int way = find_way(set, tag);
    if (way >= 0) {
        // [失效] 延迟失效旁路比对(与 PT Cache 同构, LIB 为空时 O(1) 短路)
        if (lib_ != nullptr && !lib_->empty()) {
            uint8_t lib_vn = 0;
            const uint8_t cl_vn = cache_array_[set][way].vn;
            if (lib_->match(tag.gscid, tag.pscid, lib_vn) && cl_vn < lib_vn) {
                cache_array_[set][way].invalidate();
                if (replacement_) replacement_->invalidate(set, static_cast<uint32_t>(way));
                stats_.record_invalidation(cache_name_);
                stats_.record_miss(cache_name_);
                stats_.record_lookup(cache_name_);
                stats_.record_latency(cache_name_, op_latency_ns);
                printf("[WALKER_LAZY_INVAL] %s stale CL dropped on lookup (gscid=%u, pscid=%u, va_seg=0x%lx, CL.VN=%u < LIB.VN=%u)\n",
                       cache_name_.c_str(), tag.gscid, tag.pscid,
                       (unsigned long)tag.va_segment, cl_vn, lib_vn);
                fflush(stdout);
                return false;
            }
        }

        out_data = cache_array_[set][way].data;
        if (replacement_) replacement_->access(set, way);
        if (cache_array_[set][way].from_prefetch) {
            stats_.record_prefetch_hit(cache_name_);
            cache_array_[set][way].from_prefetch = false;
        }
        cache_array_[set][way].access_count++;
        stats_.record_hit(cache_name_);
        stats_.record_lookup(cache_name_);
        stats_.record_latency(cache_name_, op_latency_ns);
        return true;
    }

    stats_.record_miss(cache_name_);
    stats_.record_lookup(cache_name_);
    stats_.record_latency(cache_name_, op_latency_ns);
    return false;
}

// [多RAM] 原子段更新: 纯功能(无wait/无仲裁), 逻辑与 update_entry 一致
bool WalkerSubCache::update_entry_ram(const WalkerTag& tag,
                                      const WalkerData& data,
                                      bool direct_write,
                                      sc_time& ram_latency) {
    // 跳过无效数据的缓存写入, 防止缓存污染(与 update_entry 一致)
    if (!data.reserved.valid) {
        ram_latency = SC_ZERO_TIME;
        return false;
    }

    const uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    if (direct_write) {
        const uint32_t way = 0;
        if (cache_array_[set][way].valid && !(cache_array_[set][way].tag == tag)) {
            stats_.record_eviction(cache_name_);
        }
        cache_array_[set][way].fill(tag, data, false);
        cache_array_[set][way].vn = current_vn();  // [失效] 记录全局 VN
        if (replacement_) replacement_->access(set, way);
        ram_latency = cycles_to_time(cfg_.write_way_latency_cycles);
        stats_.record_latency(cache_name_,
            (cycles_to_time(cfg_.hash_latency_cycles) + ram_latency).to_seconds() * 1e9);
        return true;
    }

    const uint32_t base_cycles = cfg_.read_set_latency_cycles +
                                 cfg_.write_way_latency_cycles;

    const int existing_way = find_way(set, tag);
    if (existing_way >= 0) {
        cache_array_[set][existing_way].data = data;
        cache_array_[set][existing_way].valid = true;
        cache_array_[set][existing_way].from_prefetch = false;
        cache_array_[set][existing_way].access_count = 0;
        cache_array_[set][existing_way].vn = current_vn();  // [失效] 记录全局 VN
        if (replacement_) replacement_->access(set, static_cast<uint32_t>(existing_way));
        ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_hit_cycles);
        stats_.record_latency(cache_name_,
            (cycles_to_time(cfg_.hash_latency_cycles) + ram_latency).to_seconds() * 1e9);
        return true;
    }

    const int empty_way = find_empty_way(set);
    if (empty_way >= 0) {
        cache_array_[set][empty_way].fill(tag, data, false);
        cache_array_[set][empty_way].vn = current_vn();  // [失效] 记录全局 VN
        if (replacement_) replacement_->access(set, static_cast<uint32_t>(empty_way));
        ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_invalid_cycles);
        stats_.record_latency(cache_name_,
            (cycles_to_time(cfg_.hash_latency_cycles) + ram_latency).to_seconds() * 1e9);
        return true;
    }

    uint32_t victim_way = 0;
    if (replacement_) {
        victim_way = replacement_->find_victim(set);
    }
    if (cache_array_[set][victim_way].valid) {
        stats_.record_eviction(cache_name_);
    }
    cache_array_[set][victim_way].fill(tag, data, false);
    cache_array_[set][victim_way].vn = current_vn();  // [失效] 记录全局 VN
    if (replacement_) {
        auto* srrip = dynamic_cast<SRRIPPolicy*>(replacement_.get());
        if (srrip) {
            srrip->on_insert(set, victim_way);
        } else {
            replacement_->access(set, victim_way);
        }
    }
    ram_latency = cycles_to_time(base_cycles + cfg_.fill_compute_index_replacement_cycles);
    stats_.record_latency(cache_name_,
        (cycles_to_time(cfg_.hash_latency_cycles) + ram_latency).to_seconds() * 1e9);
    return true;
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

// [S2] S2 Cache子表查询: 使用独立的s2_cache_array_，与普通Walker Cache完全隔离
bool WalkerSubCache::lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                               bool sv48_flag, bool x4_mode_flag,
                               WalkerData& out_data, sc_time& latency) {
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.va_segment = WalkerCache::extract_addr_segment(
        gpa, level_, false, sv48_flag, x4_mode_flag);
    tag.level = level_;
    tag.va_pa_flag = false;
    tag.stage_flag = true;
    tag.sv48_flag = sv48_flag;
    tag.x4_mode_flag = x4_mode_flag;
    tag.is_s2 = true;

    // 在s2_cache_array_中查找
    const uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    for (uint32_t w = 0; w < num_ways_; ++w) {
        auto& line = s2_cache_array_[set][w];
        if (line.valid && line.tag == tag) {
            out_data = line.data;
            latency = SC_ZERO_TIME;
            return true;
        }
    }
    latency = SC_ZERO_TIME;
    return false;
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
        uint32_t affected = invalidate_all_entries(latency);
        // [S2] 全局失效时也失效S2阵列
        affected += invalidate_s2_global();
        return affected;
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

    uint32_t affected = invalidate_scan_by_line(
        predicate,
        [](const WalkerTag&, const WalkerData&) {},
        latency);
    // [S2] VMA失效时也失效S2阵列中对应gscid的条目
    if (has_gscid) {
        affected += invalidate_s2_by_gscid(gscid);
    }
    return affected;
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
    uint32_t affected = invalidate_all_entries(latency);
    // [S2] 同时失效S2阵列
    for (auto& s : s2_cache_array_)
        for (auto& l : s)
            if (l.valid) { l.invalidate(); affected++; }
    return affected;
}

// [S2] S2 Cache独立更新: 使用s2_cache_array_，不干扰普通Walker Cache
WalkerSubCache::UpdateResult
WalkerSubCache::update_entry_s2(const WalkerTag& tag, const WalkerData& data) {
    UpdateResult result;
    if (!data.reserved.valid) {
        result.updated = false;
        result.latency = SC_ZERO_TIME;
        return result;
    }

    const uint32_t set = hash_function(tag);
    assert(set < num_sets_);

    // 1. 先查找是否已有匹配条目（更新已有数据）
    for (uint32_t w = 0; w < num_ways_; ++w) {
        if (s2_cache_array_[set][w].valid && s2_cache_array_[set][w].tag == tag) {
            s2_cache_array_[set][w].data = data;
            s2_cache_array_[set][w].valid = true;
            s2_cache_array_[set][w].vn = current_vn();  // [失效] 记录全局 VN
            result.updated = true;
            result.latency = SC_ZERO_TIME;
            return result;
        }
    }
    // 2. 查找空way
    for (uint32_t w = 0; w < num_ways_; ++w) {
        if (!s2_cache_array_[set][w].valid) {
            s2_cache_array_[set][w].fill(tag, data, false);
            s2_cache_array_[set][w].vn = current_vn();  // [失效] 记录全局 VN
            result.updated = true;
            result.latency = SC_ZERO_TIME;
            return result;
        }
    }
    // 3. 全满: 使用替换策略（只在S2阵列内选择victim）
    uint32_t victim_way = 0;
    if (replacement_) {
        victim_way = replacement_->find_victim(set);
    }
    s2_cache_array_[set][victim_way].fill(tag, data, false);
    s2_cache_array_[set][victim_way].vn = current_vn();  // [失效] 记录全局 VN
    if (replacement_) {
        auto* srrip = dynamic_cast<SRRIPPolicy*>(replacement_.get());
        if (srrip) srrip->on_insert(set, victim_way);
        else replacement_->access(set, victim_way);
    }
    result.updated = true;
    result.latency = SC_ZERO_TIME;
    return result;
}

// [S2] S2 Cache按gscid失效
uint32_t WalkerSubCache::invalidate_s2_by_gscid(gscid_t gscid) {
    return invalidate_s2_entries([gscid](const WalkerTag& tag) {
        return tag.gscid == gscid;
    });
}

// [S2] S2 Cache全局失效
uint32_t WalkerSubCache::invalidate_s2_global() {
    uint32_t affected = 0;
    for (auto& s : s2_cache_array_)
        for (auto& l : s)
            if (l.valid) { l.invalidate(); affected++; }
    return affected;
}

// [S2] S2 Cache通用失效扫描
uint32_t WalkerSubCache::invalidate_s2_entries(
    std::function<bool(const WalkerTag&)> predicate) {
    uint32_t affected = 0;
    for (auto& s : s2_cache_array_)
        for (auto& l : s)
            if (l.valid && predicate(l.tag)) { l.invalidate(); affected++; }
    return affected;
}

// ============================================================
// [失效][多RAM] WalkerSubCache 候选 set 枚举与失效原子段
// ============================================================

// 小范围枚举: 枚举 temp1=000..111, 与本级 addr 段的 temp2 异或得到
// 8 个 Cache set 索引 (legacy 哈希下无法枚举 -> 返回0, 调用方降级全表扫描)
uint32_t WalkerSubCache::enum_candidate_sets(iova_t addr, bool addr_is_va,
                                             bool sv48, bool x4_mode,
                                             uint32_t* out_sets) const {
    if (!hash_v2_ || out_sets == nullptr) return 0;

    const iova_t va_seg = WalkerCache::extract_addr_segment(
        addr, level_, addr_is_va, sv48, x4_mode);
    const uint32_t temp2 = hash_temp2(va_seg);
    uint32_t count = 0;
    for (uint32_t temp1 = 0; temp1 < 8; ++temp1) {
        const uint32_t set = set_from_raw(hash_combine(temp1, temp2));
        bool dup = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (out_sets[i] == set) { dup = true; break; }
        }
        if (!dup) out_sets[count++] = set;
    }
    return count;
}

// 失效谓词:
//  - VMA: 按 gscid/pscid/addr(本级段) 匹配; is_s2 条目不属于 VMA 范围
//  - GVMA: 按 gscid 匹配(两阶段下 GPA 无法精准索引, 统一转为 gscid 扫表)
bool WalkerSubCache::line_matches_inval(const WalkerTag& tag,
                                        const CacheMessage& cmd) const {
    if (cmd.invalidate_mode == CacheInvalidateMode::GLOBAL) return true;

    if (cmd.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        if (cmd.has_gscid && tag.gscid != cmd.gscid) return false;
        return true;
    }

    if (cmd.has_gscid && tag.gscid != cmd.gscid) return false;
    if (cmd.has_pscid && tag.pscid != cmd.pscid) return false;
    if (cmd.has_iova) {
        const iova_t seg = WalkerCache::extract_addr_segment(
            cmd.iova, tag.level, tag.va_pa_flag, tag.sv48_flag, tag.x4_mode_flag);
        if (tag.va_segment != seg) return false;
    }
    return true;
}

// 单 set 失效原子段 (PRECISE / SCAN_RANGE 子失效)
uint32_t WalkerSubCache::invalidate_set_ram(uint32_t set, const CacheMessage& cmd,
                                            sc_time& ram_latency) {
    uint32_t affected = invalidate_set_functional(
        set,
        [&](const WalkerTag& tag, const WalkerData&) {
            return line_matches_inval(tag, cmd);
        },
        ram_latency);
    // S2 阵列与普通阵列共用 set 索引空间, 同步处理本 set
    affected += invalidate_s2_range(set, set + 1, cmd);
    return affected;
}

// 本 RAM set 区间扫描失效原子段 (SCAN / GLOBAL 子失效)
uint32_t WalkerSubCache::invalidate_ram_range(uint32_t ram_id,
                                              const CacheMessage& cmd,
                                              sc_time& ram_latency) {
    const uint32_t begin = ram_set_begin(ram_id);
    const uint32_t end = ram_set_end(ram_id);

    uint32_t affected = invalidate_range_functional(
        begin, end,
        [&](const WalkerTag& tag, const WalkerData&) {
            return line_matches_inval(tag, cmd);
        },
        ram_latency);
    affected += invalidate_s2_range(begin, end, cmd);
    return affected;
}

// LIB 批量扫表原子段 (VN 回绕触发)
uint32_t WalkerSubCache::lazy_sweep_ram(uint32_t ram_id, uint8_t new_vn,
                                        sc_time& ram_latency) {
    const uint32_t begin = ram_set_begin(ram_id);
    const uint32_t end = ram_set_end(ram_id);
    uint32_t affected = lazy_sweep_range_functional(
        begin, end, new_vn,
        [](const WalkerTag& tag) { return tag.gscid; },
        [](const WalkerTag& tag) { return tag.pscid; },
        ram_latency);
    affected += lazy_sweep_s2_range(begin, end, new_vn);
    return affected;
}

// [失效] S2 阵列区间失效 (S2 条目为 G-stage 显式第二阶段缓存, 按 gscid 失效)
uint32_t WalkerSubCache::invalidate_s2_range(uint32_t set_begin, uint32_t set_end,
                                             const CacheMessage& cmd) {
    if (set_end > num_sets_) set_end = num_sets_;
    uint32_t affected = 0;
    const bool global = (cmd.invalidate_mode == CacheInvalidateMode::GLOBAL);
    for (uint32_t s = set_begin; s < set_end; ++s) {
        for (uint32_t w = 0; w < num_ways_; ++w) {
            auto& line = s2_cache_array_[s][w];
            if (!line.valid) continue;
            if (!global && cmd.has_gscid && line.tag.gscid != cmd.gscid) continue;
            line.invalidate();
            stats_.record_invalidation(cache_name_);
            affected++;
        }
    }
    return affected;
}

// [失效] S2 阵列 LIB 批量扫表 (与普通阵列同一套 VN 语义)
uint32_t WalkerSubCache::lazy_sweep_s2_range(uint32_t set_begin, uint32_t set_end,
                                             uint8_t new_vn) {
    if (lib_ == nullptr) return 0;
    if (set_end > num_sets_) set_end = num_sets_;
    uint32_t affected = 0;
    for (uint32_t s = set_begin; s < set_end; ++s) {
        for (uint32_t w = 0; w < num_ways_; ++w) {
            auto& line = s2_cache_array_[s][w];
            if (!line.valid) continue;
            uint8_t lib_vn = 0;
            if (!lib_->match(line.tag.gscid, line.tag.pscid, lib_vn)) {
                line.vn = new_vn;
                continue;
            }
            if (line.vn < lib_vn) {
                line.invalidate();
                stats_.record_invalidation(cache_name_);
                affected++;
            } else {
                line.vn = new_vn;
            }
        }
    }
    return affected;
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

    // [多RAM] 三子表独立分RAM: 各自按本级set哈希低位分组;
    // 要求三子表num_rams一致(简化subsystem的RAM worker拓扑)
    assert(cfg1.num_rams == cfg2.num_rams && cfg2.num_rams == cfg3.num_rams &&
           "walker ptw_c1/c2/c3 num_rams must be identical");
    ptw_c1_->configure_multi_ram(cfg1.num_rams);
    ptw_c2_->configure_multi_ram(cfg2.num_rams);
    ptw_c3_->configure_multi_ram(cfg3.num_rams);
    num_rams_ = (cfg3.num_rams == 0) ? 1 : cfg3.num_rams;
    ram_fifo_depth_ = cfg3.ram_fifo_depth;

    // 上电主动无效化所有条目（确保初始状态干净，避免残留数据污染）
    uint32_t cleared = 0;
    cleared += ptw_c1_->invalidate_global(nullptr);
    cleared += ptw_c2_->invalidate_global(nullptr);
    cleared += ptw_c3_->invalidate_global(nullptr);
    printf("[WALKER_CACHE] POWER-ON RESET: all entries invalidated (cleared=%u)\n", cleared);
    fflush(stdout);
}

WalkerCache::~WalkerCache() = default;

// [多RAM] 根据level选择子表
WalkerSubCache* WalkerCache::sub_cache(uint8_t level) const {
    switch (level) {
        case 1: return ptw_c1_.get();
        case 2: return ptw_c2_.get();
        case 3: return ptw_c3_.get();
        default: return nullptr;
    }
}

// [多RAM] 计算某级子操作的目标RAM组号(无延时)
uint32_t WalkerCache::compute_ram_id(uint8_t level, gscid_t gscid,
                                     pscid_t pscid, iova_t addr,
                                     bool va_pa_flag, bool stage_flag,
                                     bool sv48_flag, bool x4_mode_flag) const {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) return 0;
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.level = level;
    tag.va_pa_flag = va_pa_flag;
    tag.stage_flag = stage_flag;
    tag.sv48_flag = sv48_flag;
    tag.x4_mode_flag = x4_mode_flag;
    tag.va_segment = extract_addr_segment(addr, level, va_pa_flag,
                                          sv48_flag, x4_mode_flag);
    return sub->compute_ram_id(tag);
}

// [多RAM] 单级子表原子段查询(无wait), RAM worker消耗 ram_latency
bool WalkerCache::lookup_level_ram(uint8_t level, gscid_t gscid,
                                   pscid_t pscid, iova_t va,
                                   bool va_pa_flag, bool stage_flag,
                                   bool sv48_flag, bool x4_mode_flag,
                                   WalkerData& out_data, sc_time& ram_latency) {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) { ram_latency = SC_ZERO_TIME; return false; }
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.level = level;
    tag.va_pa_flag = va_pa_flag;
    tag.stage_flag = stage_flag;
    tag.sv48_flag = sv48_flag;
    tag.x4_mode_flag = x4_mode_flag;
    tag.is_s2 = false;
    tag.va_segment = extract_addr_segment(va, level, va_pa_flag,
                                          sv48_flag, x4_mode_flag);
    return sub->lookup_ram(tag, out_data, ram_latency);
}

// [多RAM] 单级子表原子段更新(无wait); level=1 direct_write(直接映射)
bool WalkerCache::update_level_ram(uint8_t level, gscid_t gscid,
                                   pscid_t pscid, iova_t va,
                                   const WalkerData& data,
                                   sc_time& ram_latency) {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) { ram_latency = SC_ZERO_TIME; return false; }
    WalkerTag tag;
    tag.gscid = gscid;
    tag.pscid = pscid;
    tag.level = level;
    tag.va_pa_flag = walker_va_pa_flag(data);
    tag.stage_flag = walker_stage_flag(data);
    tag.sv48_flag = walker_sv48_flag(data);
    tag.x4_mode_flag = walker_x4_mode_flag(data);
    tag.is_s2 = false;
    tag.va_segment = extract_addr_segment(
        va, level, tag.va_pa_flag, tag.sv48_flag, tag.x4_mode_flag);
    return sub->update_entry_ram(tag, data, level == 1, ram_latency);
}

// [多RAM] join仲裁后记录VS lookup统计(与旧lookup()口径一致)
void WalkerCache::record_vs_lookup_result(uint8_t hit_level, bool is_leaf) {
    vs_lookup_count_++;
    switch (hit_level) {
        case 3: vs_hit_c3_count_++; break;
        case 2: vs_hit_c2_count_++; break;
        case 1: vs_hit_c1_count_++; break;
        default: vs_miss_count_++; break;
    }
    // [大页] 端到端leaf命中单独计数
    if (hit_level > 0 && is_leaf) vs_leaf_hit_count_++;
}

// [多RAM] hash线程拆分update时记录更新类型统计(与旧update()口径一致)
void WalkerCache::record_update_kind(WalkerUpdateKind kind) {
    switch (kind) {
        case WalkerUpdateKind::PTWC_1_2_3: update_ptwc_123_count_++; break;
        case WalkerUpdateKind::PTWC_2_3:   update_ptwc_23_count_++;  break;
        case WalkerUpdateKind::PTWC_3:     update_ptwc_3_count_++;   break;
        case WalkerUpdateKind::NONE:       update_none_count_++;     break;
        default: break;
    }
}

bool WalkerCache::lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                         bool va_pa_flag, bool stage_flag,
                         bool sv48_flag, bool x4_mode_flag,
                         WalkerData& out_data, uint8_t& hit_level,
                         sc_time& latency) {
    latency = SC_ZERO_TIME;
    hit_level = 0;
    vs_lookup_count_++;
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
        vs_hit_c3_count_++;
    } else if (hit_c2) {
        out_data = data_c2;
        hit_level = 2;
        latency = lat_c2;
        vs_hit_c2_count_++;
    } else if (hit_c1) {
        out_data = data_c1;
        hit_level = 1;
        latency = lat_c1;
        vs_hit_c1_count_++;
    } else {
        // 全未命中：延时累加
        latency = lat_c3 + lat_c2 + lat_c1;
        vs_miss_count_++;
    }
    // [大页] VS端到端leaf命中统计
    if ((hit_c3 || hit_c2 || hit_c1) && walker_is_leaf(out_data)) vs_leaf_hit_count_++;
    
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
    // [大页] S2端到端leaf命中统计
    if ((hit_c3 || hit_c2 || hit_c1) && walker_is_leaf(out_data)) s2_leaf_hit_count_++;

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

    // 串行更新（S2 Cache使用独立的s2_cache_array_，不干扰普通Walker Cache）
    if (update_ptwc1 && !sv39_mode_ && walker_sv48_flag(ptwc1_data)) {
        auto t1 = make_tag(1, ptwc1_data);
        auto r1 = ptw_c1_->update_entry_s2(t1, ptwc1_data);
        merge_result(result.updated_ptwc1, r1);
    }

    if (update_ptwc2) {
        auto t2 = make_tag(2, ptwc2_data);
        auto r2 = ptw_c2_->update_entry_s2(t2, ptwc2_data);
        merge_result(result.updated_ptwc2, r2);
    }

    {
        auto t3 = make_tag(3, ptwc3_data);
        auto r3 = ptw_c3_->update_entry_s2(t3, ptwc3_data);
        merge_result(result.updated_ptwc3, r3);
    }

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
    // [S2] 同时失效S2阵列
    affected += ptw_c1_->invalidate_s2_by_gscid(gscid);
    affected += ptw_c2_->invalidate_s2_by_gscid(gscid);
    affected += ptw_c3_->invalidate_s2_by_gscid(gscid);
    return affected;
}

uint32_t WalkerCache::invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                                sc_time* latency) {
    uint32_t affected = 0;
    affected += ptw_c1_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    affected += ptw_c2_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    affected += ptw_c3_->invalidate_by_gscid_pscid(gscid, pscid, latency);
    // [S2] S2阵列也按gscid失效（S2条目不含pscid，按gscid失效即可）
    affected += ptw_c1_->invalidate_s2_by_gscid(gscid);
    affected += ptw_c2_->invalidate_s2_by_gscid(gscid);
    affected += ptw_c3_->invalidate_s2_by_gscid(gscid);
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
    // invalidate_global已在WalkerSubCache内包含S2阵列失效
    return affected;
}

void WalkerCache::set_clock_period(const sc_time& clk) {
    ptw_c1_->set_clock_period(clk);
    ptw_c2_->set_clock_period(clk);
    ptw_c3_->set_clock_period(clk);
}

// ============================================================
// [失效][多RAM] WalkerCache 失效子操作分发层 (按 level 选子表)
// ============================================================

void WalkerCache::set_lazy_invalid_buffer(const LazyInvalidBuffer* lib) {
    ptw_c1_->set_lazy_invalid_buffer(lib);
    ptw_c2_->set_lazy_invalid_buffer(lib);
    ptw_c3_->set_lazy_invalid_buffer(lib);
}

uint32_t WalkerCache::enum_candidate_sets(uint8_t level, iova_t addr,
                                          bool addr_is_va, bool sv48,
                                          bool x4_mode,
                                          uint32_t* out_sets) const {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) return 0;
    return sub->enum_candidate_sets(addr, addr_is_va, sv48, x4_mode, out_sets);
}

uint32_t WalkerCache::ram_of_set(uint8_t level, uint32_t set) const {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) return 0;
    return sub->ram_of_set(set);
}

uint32_t WalkerCache::invalidate_set_ram(uint8_t level, uint32_t set,
                                         const CacheMessage& cmd,
                                         sc_time& ram_latency) {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) { ram_latency = SC_ZERO_TIME; return 0; }
    return sub->invalidate_set_ram(set, cmd, ram_latency);
}

uint32_t WalkerCache::invalidate_ram_range(uint8_t level, uint32_t ram_id,
                                           const CacheMessage& cmd,
                                           sc_time& ram_latency) {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) { ram_latency = SC_ZERO_TIME; return 0; }
    return sub->invalidate_ram_range(ram_id, cmd, ram_latency);
}

uint32_t WalkerCache::lazy_sweep_ram(uint8_t level, uint32_t ram_id,
                                     uint8_t new_vn, sc_time& ram_latency) {
    WalkerSubCache* sub = sub_cache(level);
    if (!sub) { ram_latency = SC_ZERO_TIME; return 0; }
    return sub->lazy_sweep_ram(ram_id, new_vn, ram_latency);
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
        if (level == 3) {
            return (root_l2 << 9) | extract_bits(addr, 29, 21);
        }
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
