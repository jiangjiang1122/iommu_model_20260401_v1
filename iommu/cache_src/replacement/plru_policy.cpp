#include "replacement/plru_policy.h"
#include <cassert>
#include <cmath>

namespace iommu {

PLRUPolicy::PLRUPolicy(uint32_t num_sets, uint32_t num_ways)
    : num_sets_(num_sets), num_ways_(num_ways), tree_bits_(num_ways - 1)
{
    assert(num_ways >= 1 && (num_ways & (num_ways - 1)) == 0 &&
           "num_ways must be a power of 2");
    tree_.resize(num_sets, std::vector<uint8_t>(tree_bits_, 0));
}

void PLRUPolicy::access(uint32_t set, uint32_t way) {
    assert(set < num_sets_ && way < num_ways_);
    update_tree(set, way);
}

uint32_t PLRUPolicy::find_victim(uint32_t set) {
    assert(set < num_sets_);
    return traverse_tree(set);
}

void PLRUPolicy::invalidate(uint32_t set, uint32_t way) {
    assert(set < num_sets_ && way < num_ways_);
    // 失效时，将该 way 设为"最容易被替换"
    // 即沿路径设置 tree bits 使其指向该 way
    if (num_ways_ == 1) return;

    uint32_t depth = static_cast<uint32_t>(std::log2(num_ways_));
    uint32_t node = 0;
    for (uint32_t level = 0; level < depth; ++level) {
        uint32_t bit_in_way = (way >> (depth - 1 - level)) & 1;
        // 将 tree bit 设置为指向该 way 的方向 (即"不是最近访问的")
        // tree bit = 0 表示最近访问右子树，淘汰选左
        // tree bit = 1 表示最近访问左子树，淘汰选右
        // 要让淘汰选到 way，需要沿路径设置指向 way 的方向
        if (bit_in_way == 0) {
            // way在左子树，需要淘汰选左 → tree bit = 0 (最近访问右)
            tree_[set][node] = 0;
        } else {
            // way在右子树，需要淘汰选右 → tree bit = 1 (最近访问左)
            tree_[set][node] = 1;
        }
        node = 2 * node + 1 + bit_in_way;
    }
}

void PLRUPolicy::reset() {
    for (auto& t : tree_) {
        std::fill(t.begin(), t.end(), 0);
    }
}

void PLRUPolicy::update_tree(uint32_t set, uint32_t way) {
    // 二叉树 pLRU：访问 way 时，沿路径设置 tree bits 指向"远离"该 way 的方向
    // tree bit = 1 表示最近访问了左子树（淘汰时选右子树）
    // tree bit = 0 表示最近访问了右子树（淘汰时选左子树）
    if (num_ways_ == 1) return;

    uint32_t depth = static_cast<uint32_t>(std::log2(num_ways_));
    uint32_t node = 0;

    for (uint32_t level = 0; level < depth; ++level) {
        uint32_t bit_in_way = (way >> (depth - 1 - level)) & 1;
        if (bit_in_way == 0) {
            // way 在左子树 → 设置 bit=1 表示最近访问了左子树
            tree_[set][node] = 1;
        } else {
            // way 在右子树 → 设置 bit=0 表示最近访问了右子树
            tree_[set][node] = 0;
        }
        node = 2 * node + 1 + bit_in_way;
    }
}

uint32_t PLRUPolicy::traverse_tree(uint32_t set) const {
    // 沿 tree bits 找到"最久未访问"的 way
    // bit=1 → 最近访问了左子树，淘汰从右子树选 → 走右 (bit=1)
    // bit=0 → 最近访问了右子树，淘汰从左子树选 → 走左 (bit=0)
    if (num_ways_ == 1) return 0;

    uint32_t depth = static_cast<uint32_t>(std::log2(num_ways_));
    uint32_t node = 0;
    uint32_t way = 0;

    for (uint32_t level = 0; level < depth; ++level) {
        way <<= 1;
        if (tree_[set][node] == 1) {
            // 最近访问了左子树 → 淘汰从右子树选
            way |= 1;
            node = 2 * node + 2;  // 右子节点
        } else {
            // 最近访问了右子树 → 淘汰从左子树选
            node = 2 * node + 1;  // 左子节点
        }
    }
    return way;
}

} // namespace iommu
