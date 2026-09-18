#include "cache/dedup_cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace iommu {

DedupCache::DedupCache(uint32_t num_sets, uint32_t num_ways)
    : num_sets_(num_sets),
      num_ways_(num_ways),
      cache_array_(num_sets, std::vector<DedupCacheLine>(num_ways)) {
}

// [多RAM] 配置 RAM 分组参数: num_rams 必须为2的幂且整除 num_sets
void DedupCache::configure_multi_ram(uint32_t num_rams) {
    num_rams_ = (num_rams == 0) ? 1 : num_rams;
    assert((num_rams_ & (num_rams_ - 1)) == 0 && "dedup num_rams must be power of 2");
    assert(num_sets_ % num_rams_ == 0 && "dedup num_rams must divide num_sets");
    sets_per_ram_ = num_sets_ / num_rams_;
    log2_num_rams_ = 0;
    for (uint32_t v = num_rams_; v > 1; v >>= 1) log2_num_rams_++;
    printf("[DEDUP_CACHE] Multi-RAM config: num_rams=%u, sets_per_ram=%u\n",
           num_rams_, sets_per_ram_);
}

// [多RAM] 未掩码原始hash(与 PT Cache 同构: gscid/pscid/iova>>12)
uint32_t DedupCache::raw_hash(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 32) - 1;  // 32-bit page number
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    // 与 PT Cache 一致: IOVA 右移12位(去掉4KB页内偏移), 避免页对齐IOVA全部落到Set 0
    __uint128_t array =
        (static_cast<__uint128_t>(gscid) << 64) |
        (static_cast<__uint128_t>((iova >> 12) & iova_mask) << 20) |
        static_cast<__uint128_t>(pscid & pscid_mask);
    __uint128_t temp1 = array ^ (array >> 40);
    __uint128_t temp2 = temp1 ^ (temp1 >> 20);

    return static_cast<uint32_t>(temp2);
}

// [多RAM] 计算任务所属 RAM 组号: org_hash & (num_rams-1)
uint32_t DedupCache::compute_ram_id(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    if (num_rams_ <= 1) return 0;
    return raw_hash(gscid, pscid, align_iova_4k(iova)) & (num_rams_ - 1);
}

uint32_t DedupCache::hash_function(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    // [多RAM] 新映射: 低 log2(num_rams) 位选 RAM, 高位作 RAM 内索引
    // global_set = ram_id * sets_per_ram + set_in_ram, RAM i 独占连续 set 区间
    // num_rams=1 时退化为原 raw & (num_sets-1)
    uint32_t raw = raw_hash(gscid, pscid, iova);
    if (num_rams_ <= 1) {
        return raw & (num_sets_ - 1);
    }
    uint32_t ram_id = raw & (num_rams_ - 1);
    uint32_t set_in_ram = (raw >> log2_num_rams_) & (sets_per_ram_ - 1);
    return ram_id * sets_per_ram_ + set_in_ram;
}

DedupCacheLine* DedupCache::lookup(gscid_t gscid, pscid_t pscid, iova_t iova) {
    iova_t iova_aligned = align_iova_4k(iova);
    uint32_t set = hash_function(gscid, pscid, iova_aligned);
    for (uint32_t w = 0; w < num_ways_; w++) {
        DedupCacheLine& line = cache_array_[set][w];
        if (line.valid &&
            line.gscid == gscid &&
            line.pscid == pscid &&
            line.iova == iova_aligned) {
            return &line;
        }
    }
    return nullptr;
}

bool DedupCache::insert(gscid_t gscid, pscid_t pscid, iova_t iova,
                        uint16_t head_index, bool is_req) {
    iova_t iova_aligned = align_iova_4k(iova);
    uint32_t set = hash_function(gscid, pscid, iova_aligned);

    auto write_way = [&](uint32_t w) {
        DedupCacheLine& line = cache_array_[set][w];
        line.valid = true;
        line.gscid = gscid;
        line.pscid = pscid;
        line.iova = iova_aligned;
        line.head_index = head_index;
        line.is_req = is_req;
    };

    // 步骤1: 有 V=0 的 way -> 直接写入
    for (uint32_t w = 0; w < num_ways_; w++) {
        if (!cache_array_[set][w].valid) {
            write_way(w);
            return true;
        }
    }

    // 步骤iii: 有 V=1 & is_req=0(预取占位) 的 way -> 随机淘汰其一并写入
    //   (架构定义的条件替换: 仅允许淘汰预取占位; is_req=1 主占位受保护不可替换)
    uint32_t victim_count = 0;
    for (uint32_t w = 0; w < num_ways_; w++) {
        if (cache_array_[set][w].valid && !cache_array_[set][w].is_req) {
            victim_count++;
        }
    }
    if (victim_count > 0) {
        uint32_t pick = static_cast<uint32_t>(std::rand()) % victim_count;
        uint32_t seen = 0;
        for (uint32_t w = 0; w < num_ways_; w++) {
            if (cache_array_[set][w].valid && !cache_array_[set][w].is_req) {
                if (seen == pick) {
                    write_way(w);
                    return true;
                }
                seen++;
            }
        }
    }

    // 步骤iv: 无空位置可写(全为 V=1 & is_req=1) = hash冲突 -> 插入失败,
    //   调用方(execute_dedup_request)将任务 bypass 直转 PTW(Walk), 不记录在 Cache 和 Buffer 中
    return false;
}

// [hash冲突判定] 目标 set 是否还有可写位置: V=0 空 way 或 V=1&is_req=0 可淘汰预取占位 (无延时预检)
bool DedupCache::set_has_writable_way(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    iova_t iova_aligned = align_iova_4k(iova);
    uint32_t set = hash_function(gscid, pscid, iova_aligned);
    for (uint32_t w = 0; w < num_ways_; w++) {
        const DedupCacheLine& line = cache_array_[set][w];
        if (!line.valid || !line.is_req) return true;  // V=0 空位 或 可淘汰的预取占位
    }
    return false;  // 全为 V=1 & is_req=1 -> hash冲突
}

void DedupCache::clear_line(DedupCacheLine* line) {
    if (line != nullptr) {
        line->clear();
    }
}

} // namespace iommu
