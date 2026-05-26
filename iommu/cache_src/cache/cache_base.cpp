#include "cache/cache_base.h"

// CacheBase 是模板类，主要实现在头文件中
// 此文件用于：
// 1. 确保 replacement_policy 工厂方法的实现
// 2. 显式实例化常用模板 (可选)

namespace iommu {

std::unique_ptr<ReplacementPolicy> ReplacementPolicy::create(
    const std::string& algo_name,
    uint32_t num_sets,
    uint32_t num_ways,
    uint32_t srrip_m_bits)
{
    if (algo_name == "srrip") {
        return std::make_unique<SRRIPPolicy>(num_sets, num_ways, srrip_m_bits);
    } else if (algo_name == "plru") {
        return std::make_unique<PLRUPolicy>(num_sets, num_ways);
    } else if (algo_name == "none" && num_ways <= 1) {
        return nullptr;  // 直接映射不需要替换算法
    }
    // 默认 pLRU
    return std::make_unique<PLRUPolicy>(num_sets, num_ways);
}

} // namespace iommu
