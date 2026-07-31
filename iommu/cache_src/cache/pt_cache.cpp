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
    // [失效] 新哈希需要 log2(num_sets) 作为 temp1 的移位量
    assert((num_sets_ & (num_sets_ - 1)) == 0 && "num_sets must be power of 2");
    log2_num_sets_ = 0;
    for (uint32_t v = num_sets_; v > 1; v >>= 1) log2_num_sets_++;
    hash_v2_ = (cfg.hash_mode != "legacy");
    printf("[PT_CACHE] Multi-RAM config: num_rams=%u, sets_per_ram=%u, ram_fifo_depth=%u, hash_mode=%s\n",
           num_rams_, sets_per_ram_, cfg.ram_fifo_depth,
           hash_v2_ ? "inval_v2" : "legacy");
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

// [失效] 旧单体失效接口 invalidate_vma/invalidate_gvma/invalidate_by_gscid/
// invalidate_by_gscid_pscid/invalidate_global 已删除:
// 它们直接调用 invalidate_scan_by_line/invalidate_all_entries, 绕过了多RAM
// 原子段拆分与 LIB 延迟失效语义。现由 CacheSubsystem::dispatch_pt_invalidate
// 拆分为子失效, 经 invalidate_set_ram / invalidate_ram_range / lazy_sweep_ram
// 在各 RAM worker 内串行执行。

uint32_t PTCache::hash_function(const PTTag& tag) const {
    return set_from_raw(raw_hash(tag));
}

// [多RAM] raw hash -> 全局 set 号
// 低 log2(num_rams) 位选 RAM, 高位作 RAM 内索引
// global_set = ram_id * sets_per_ram + set_in_ram, RAM i 独占连续 set 区间
// num_rams=1 时退化为原 raw & (num_sets-1)
uint32_t PTCache::set_from_raw(uint32_t raw) const {
    if (num_rams_ <= 1) {
        return raw & (num_sets_ - 1);
    }
    uint32_t ram_id = raw & (num_rams_ - 1);
    uint32_t set_in_ram = (raw >> log2_num_rams_) & (sets_per_ram_ - 1);
    return ram_id * sets_per_ram_ + set_in_ram;
}

// [失效] 新哈希 temp1 分量: temp1 = (gscid ^ pscid) & 0b111
uint32_t PTCache::hash_temp1(gscid_t gscid, pscid_t pscid) {
    return (static_cast<uint32_t>(gscid) ^ pscid) & 0x7U;
}

// [失效] 新哈希 temp2 分量: temp2 = PN ^ (PN >> 22), PN = iova >> 12 (44bit PN)
uint32_t PTCache::hash_temp2(iova_t iova) {
    const iova_t pn = (iova >> 12) & ((static_cast<iova_t>(1) << 44) - 1);
    return static_cast<uint32_t>(pn ^ (pn >> 22));
}

// [失效] result = ((temp1 << (log2S - 3)) ^ temp2) & (S - 1)
uint32_t PTCache::hash_combine(uint32_t temp1, uint32_t temp2) const {
    const uint32_t shift = (log2_num_sets_ > 3) ? (log2_num_sets_ - 3) : 0;
    return ((temp1 << shift) ^ temp2) & (num_sets_ - 1);
}

// [多RAM] 未掩码原始hash(原 hash_function 去掉 & mask)
uint32_t PTCache::raw_hash(const PTTag& tag) const {
    if (hash_v2_) {
        // [失效] 新哈希: 哈希值主要由 iova 得到, gscid/pscid 仅影响少量比特,
        // 使得指定 addr 的失效指令可通过枚举 temp1 得到全部可能的缓存索引。
        return hash_combine(hash_temp1(tag.gscid, tag.pscid),
                            hash_temp2(tag.iova));
    }

    // legacy 哈希(保留用于回归对比与回退)
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
        // [失效] 延迟失效旁路比对: 与 RAM 读并行的 LIB CAM 匹配。
        // LIB 为空时 O(1) 短路(基线场景零开销)。
        // 命中且 CL.VN < LIB.VN 表示命中的是失效数据: 同步置 V=0 并按 MISS 返回。
        if (lib_ != nullptr && !lib_->empty()) {
            uint8_t lib_vn = 0;
            const uint8_t cl_vn = cache_array_[set][way].vn;
            if (lib_->match(tag.gscid, tag.pscid, lib_vn) && cl_vn < lib_vn) {
                cache_array_[set][way].invalidate();
                if (replacement_) replacement_->invalidate(set, static_cast<uint32_t>(way));
                stats_.record_invalidation(cache_name_);
                lazy_inval_drops_++;   // [失效] 量化延迟失效生效次数
                stats_.record_miss(cache_name_);
                stats_.record_lookup(cache_name_);
                stats_.record_phase_lookup(cache_name_, phase_tracker_);
                stats_.record_latency(cache_name_, op_latency_ns);
                printf("[PT_CACHE_LAZY_INVAL] stale CL dropped on lookup (gscid=%u, pscid=%u, iova=0x%lx, CL.VN=%u < LIB.VN=%u)\n",
                       tag.gscid, tag.pscid, (unsigned long)tag.iova,
                       cl_vn, lib_vn);
                fflush(stdout);
                return false;
            }
        }

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
        // [失效] 新 CL 写入时同步记录当前全局 VN
        cache_array_[set][existing_way].vn = current_vn();
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
        cache_array_[set][empty_way].vn = current_vn();  // [失效] 记录全局 VN
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
    cache_array_[set][victim_way].vn = current_vn();  // [失效] 记录全局 VN
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

// ============================================================
// [失效] 候选 set 计算与多RAM 失效原子段
// ============================================================

// PRECISE: 精准失效的唯一候选 set
uint32_t PTCache::precise_candidate_set(gscid_t gscid, pscid_t pscid,
                                        iova_t iova) const {
    PTTag tag = make_tag(gscid, pscid, iova, TransStage::STAGE1_AND_2, true, false);
    return hash_function(tag);
}

// SCAN_RANGE: 枚举 temp1 = 000..111, 得到该 addr 对应的全部可能缓存索引
// (仅新哈希成立; legacy 哈希下 gscid/pscid 影响全部比特, 无法枚举 ->
//  返回 0, 由调用方降级为全表扫描)
uint32_t PTCache::enum_candidate_sets(iova_t iova, uint32_t* out_sets) const {
    if (!hash_v2_ || out_sets == nullptr) return 0;

    const uint32_t temp2 = hash_temp2(align_iova(iova, PageSize::PAGE_4K));
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

// 失效谓词: 实现 IOTINVAL.VMA/GVMA 的匹配规则
//  - VMA 仅命中含第一阶段的翻译; GVMA 仅命中含第二阶段的翻译
//  - PSCV=1 时包含全局映射(PTE.G=1)的条目除外
bool PTCache::line_matches_inval(const PTTag& tag, const PTData& data,
                                 const CacheMessage& cmd) const {
    if (cmd.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        if (tag.stage == TransStage::STAGE1_ONLY) return false;
        if (cmd.has_gscid && tag.gscid != cmd.gscid) return false;
        if (cmd.has_iova &&
            tag.iova != align_iova(cmd.iova, PageSize::PAGE_4K)) return false;
        return true;
    }

    // IOTINVAL.VMA (含 DC/PC 关联失效使用的 VMA 语义)
    if (tag.stage == TransStage::STAGE2_ONLY) return false;
    if (cmd.has_gscid && tag.gscid != cmd.gscid) return false;
    if (cmd.has_pscid && tag.pscid != cmd.pscid) return false;
    if (cmd.has_iova &&
        tag.iova != align_iova(cmd.iova, PageSize::PAGE_4K)) return false;
    // PSCV=1: 全局映射条目除外
    if (cmd.has_pscid && data.vs_pte.G != 0) return false;
    return true;
}

// 单 set 失效原子段 (PRECISE / SCAN_RANGE 子失效)
uint32_t PTCache::invalidate_set_ram(uint32_t set, const CacheMessage& cmd,
                                     sc_time& ram_latency) {
    return invalidate_set_functional(
        set,
        [&](const PTTag& tag, const PTData& data) {
            return line_matches_inval(tag, data, cmd);
        },
        ram_latency);
}

// 本 RAM set 区间扫描失效原子段 (SCAN / GLOBAL 子失效)
uint32_t PTCache::invalidate_ram_range(uint32_t ram_id, const CacheMessage& cmd,
                                       sc_time& ram_latency) {
    const uint32_t begin = ram_set_begin(ram_id);
    const uint32_t end = ram_set_end(ram_id);
    const bool global = (cmd.invalidate_mode == CacheInvalidateMode::GLOBAL);

    return invalidate_range_functional(
        begin, end,
        [&](const PTTag& tag, const PTData& data) {
            if (global) return true;
            return line_matches_inval(tag, data, cmd);
        },
        ram_latency);
}

// LIB 批量扫表原子段 (VN 回绕触发)
uint32_t PTCache::lazy_sweep_ram(uint32_t ram_id, uint8_t new_vn,
                                 sc_time& ram_latency) {
    return lazy_sweep_range_functional(
        ram_set_begin(ram_id), ram_set_end(ram_id), new_vn,
        [](const PTTag& tag) { return tag.gscid; },
        [](const PTTag& tag) { return tag.pscid; },
        ram_latency);
}

} // namespace iommu
