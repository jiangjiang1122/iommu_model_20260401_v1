#include "cache/dedup_cache.h"

#include <cstdlib>

namespace iommu {

DedupCache::DedupCache(uint32_t num_sets, uint32_t num_ways)
    : num_sets_(num_sets),
      num_ways_(num_ways),
      cache_array_(num_sets, std::vector<DedupCacheLine>(num_ways)) {
}

uint32_t DedupCache::hash_function(gscid_t gscid, pscid_t pscid, iova_t iova) const {
    uint32_t mask = num_sets_ - 1;
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 32) - 1;  // 32-bit page number
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    // 与 PT Cache 一致: IOVA 右移12位(去掉4KB页内偏移), 避免页对齐IOVA全部落到Set 0
    __uint128_t array =
        (static_cast<__uint128_t>(gscid) << 64) |
        (static_cast<__uint128_t>((iova >> 12) & iova_mask) << 20) |
        static_cast<__uint128_t>(pscid & pscid_mask);
    __uint128_t temp1 = array ^ (array >> 40);
    __uint128_t temp2 = temp1 ^ (temp1 >> 20);

    return static_cast<uint32_t>(temp2) & mask;
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

    // 步骤2: 有 V=1 & is_req=0(预取占位) 的 way -> 随机淘汰其一
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

    // 步骤3: 全部为 is_req=1 受保护占位 -> 插入失败
    return false;
}

void DedupCache::clear_line(DedupCacheLine* line) {
    if (line != nullptr) {
        line->clear();
    }
}

} // namespace iommu
