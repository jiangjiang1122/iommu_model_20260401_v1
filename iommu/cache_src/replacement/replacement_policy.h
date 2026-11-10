#ifndef IOMMU_REPLACEMENT_POLICY_H
#define IOMMU_REPLACEMENT_POLICY_H

#include <cstdint>
#include <string>
#include <memory>

namespace iommu {

class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;

    // 访问通知：命中或fill后调用
    virtual void access(uint32_t set, uint32_t way) = 0;

    // 查找淘汰项：返回应被替换的 way 编号
    virtual uint32_t find_victim(uint32_t set) = 0;

    // 失效：重置指定 set/way 的状态
    virtual void invalidate(uint32_t set, uint32_t way) = 0;

    // 全局重置
    virtual void reset() = 0;

    // 算法名称
    virtual std::string name() const = 0;

    // 工厂方法
    static std::unique_ptr<ReplacementPolicy> create(
        const std::string& algo_name,
        uint32_t num_sets,
        uint32_t num_ways,
        uint32_t srrip_m_bits = 2);
};

} // namespace iommu

#endif // IOMMU_REPLACEMENT_POLICY_H
