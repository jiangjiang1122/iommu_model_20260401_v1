#ifndef IOMMU_SRRIP_POLICY_H
#define IOMMU_SRRIP_POLICY_H

#include "replacement/replacement_policy.h"
#include <vector>
#include <cstdint>

namespace iommu {

// Static RRIP (Re-Reference Interval Prediction) 替换算法
// 每个 cache line 维护 M-bit RRPV 值, 范围 [0, 2^M - 1]
// RRPV 越大越容易被替换
class SRRIPPolicy : public ReplacementPolicy {
public:
    SRRIPPolicy(uint32_t num_sets, uint32_t num_ways, uint32_t m_bits = 2);

    void access(uint32_t set, uint32_t way) override;
    uint32_t find_victim(uint32_t set) override;
    void invalidate(uint32_t set, uint32_t way) override;
    void reset() override;
    std::string name() const override { return "srrip"; }

    // 设置新插入 entry 的 RRPV 值
    void on_insert(uint32_t set, uint32_t way);

private:
    uint32_t num_sets_;
    uint32_t num_ways_;
    uint32_t m_bits_;
    uint32_t max_rrpv_;   // 2^M - 1
    // rrpv_[set][way]
    std::vector<std::vector<uint32_t>> rrpv_;
};

} // namespace iommu

#endif // IOMMU_SRRIP_POLICY_H
