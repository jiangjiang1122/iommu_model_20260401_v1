#ifndef IOMMU_PLRU_POLICY_H
#define IOMMU_PLRU_POLICY_H

#include "replacement/replacement_policy.h"
#include <vector>
#include <cstdint>

namespace iommu {

// Pseudo-LRU (Tree-based) 替换算法
// 每个 set 使用 (num_ways - 1) 个 tree bit 表示近似访问顺序
class PLRUPolicy : public ReplacementPolicy {
public:
    PLRUPolicy(uint32_t num_sets, uint32_t num_ways);

    void access(uint32_t set, uint32_t way) override;
    uint32_t find_victim(uint32_t set) override;
    void invalidate(uint32_t set, uint32_t way) override;
    void reset() override;
    std::string name() const override { return "plru"; }

private:
    uint32_t num_sets_;
    uint32_t num_ways_;
    uint32_t tree_bits_;  // num_ways - 1
    // 每个 set 的 tree bit 数组 (每bit用uint8_t存储方便操作)
    std::vector<std::vector<uint8_t>> tree_;

    // 计算 way 在二叉树中的路径，更新tree bits
    void update_tree(uint32_t set, uint32_t way);
    // 沿tree bits找到victim way
    uint32_t traverse_tree(uint32_t set) const;
};

} // namespace iommu

#endif // IOMMU_PLRU_POLICY_H
