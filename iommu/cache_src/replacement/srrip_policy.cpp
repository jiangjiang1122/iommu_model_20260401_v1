#include "replacement/srrip_policy.h"
#include <cassert>

namespace iommu {

SRRIPPolicy::SRRIPPolicy(uint32_t num_sets, uint32_t num_ways, uint32_t m_bits)
    : num_sets_(num_sets), num_ways_(num_ways), m_bits_(m_bits),
      max_rrpv_((1u << m_bits) - 1)
{
    assert(m_bits >= 1 && m_bits <= 8);
    // 初始所有 RRPV 为 max_rrpv_ (distant re-reference)
    rrpv_.resize(num_sets, std::vector<uint32_t>(num_ways, max_rrpv_));
}

void SRRIPPolicy::access(uint32_t set, uint32_t way) {
    assert(set < num_sets_ && way < num_ways_);
    // 命中时设 RRPV = 0 (near-immediate re-reference)
    rrpv_[set][way] = 0;
}

void SRRIPPolicy::on_insert(uint32_t set, uint32_t way) {
    assert(set < num_sets_ && way < num_ways_);
    // 新插入设 RRPV = max_rrpv_ - 1 (long re-reference)
    rrpv_[set][way] = max_rrpv_ - 1;
}

uint32_t SRRIPPolicy::find_victim(uint32_t set) {
    assert(set < num_sets_);

    // 循环直到找到 RRPV == max_rrpv_ 的 way
    for (uint32_t iter = 0; iter <= max_rrpv_; ++iter) {
        // 从左到右扫描寻找 RRPV == max_rrpv_
        for (uint32_t w = 0; w < num_ways_; ++w) {
            if (rrpv_[set][w] == max_rrpv_) {
                return w;
            }
        }
        // 未找到，所有 RRPV += 1
        for (uint32_t w = 0; w < num_ways_; ++w) {
            if (rrpv_[set][w] < max_rrpv_) {
                rrpv_[set][w]++;
            }
        }
    }

    // 理论上不会到达这里
    return 0;
}

void SRRIPPolicy::invalidate(uint32_t set, uint32_t way) {
    assert(set < num_sets_ && way < num_ways_);
    // 失效时设 RRPV 为最大值，使其最容易被替换
    rrpv_[set][way] = max_rrpv_;
}

void SRRIPPolicy::reset() {
    for (auto& s : rrpv_) {
        std::fill(s.begin(), s.end(), max_rrpv_);
    }
}

} // namespace iommu
