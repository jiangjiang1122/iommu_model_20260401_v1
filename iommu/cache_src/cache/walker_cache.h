#ifndef IOMMU_WALKER_CACHE_H
#define IOMMU_WALKER_CACHE_H

#include "cache/cache_base.h"
#include <memory>

namespace iommu {

class WalkerSubCache;

// Walker Cache: 内含 3 个独立子表 (PTWc_1, PTWc_2, PTWc_3)
// PTWc_1: 直接映射, 缓存第一级页表中间结果 (VPN[3] 级)
// PTWc_2: 2-way 组相连, 缓存第二级 (VPN[2] 级)
// PTWc_3: 4-way 组相连, 缓存第三级 (VPN[1] 级)
class WalkerCache : public sc_module {
public:
    SC_HAS_PROCESS(WalkerCache);

    WalkerCache(sc_module_name name,
                const CacheConfig& cfg1,
                const CacheConfig& cfg2,
                const CacheConfig& cfg3,
                StatsCollector& stats);
    ~WalkerCache() override;

    // 查询: lookup 请求不携带目标 level，内部查三级子表并按 3 > 2 > 1 选择结果。
    bool lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                bool va_pa_flag, bool stage_flag, bool sv48_flag,
                bool x4_mode_flag, WalkerData& out_data,
                uint8_t& hit_level, sc_time& latency);
    bool lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                WalkerData& out_data, uint8_t& hit_level, sc_time& latency) {
        return lookup(gscid, pscid, va, true, true, true, false,
                      out_data, hit_level, latency);
    }

    // 填充
    void fill(gscid_t gscid, pscid_t pscid, iova_t va,
              uint8_t level, const WalkerData& data);

    struct UpdateResult {
        bool    updated_ptwc1 = false;
        bool    updated_ptwc2 = false;
        bool    updated_ptwc3 = false;
        sc_time latency = SC_ZERO_TIME;
    };

    UpdateResult update(gscid_t gscid, pscid_t pscid, iova_t va,
                        WalkerUpdateKind kind,
                        const WalkerData& ptwc1_data,
                        const WalkerData& ptwc2_data,
                        const WalkerData& ptwc3_data);

    // 按 gscid 失效所有子表
    uint32_t invalidate_by_gscid(gscid_t gscid, sc_time* latency = nullptr);

    // 按 gscid + pscid 失效所有子表
    uint32_t invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                       sc_time* latency = nullptr);

    // 精确失效 (IOTINVAL.VMA 关联)
    uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                            bool has_gscid, bool has_pscid, bool has_iova,
                            CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                            sc_time* latency = nullptr);

    // GVMA 关联失效
    uint32_t invalidate_gvma(gscid_t gscid, bool has_gscid,
                             CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                             sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

    // 设置时钟周期
    void set_clock_period(const sc_time& clk);

    // 启用/关闭 PTWc_1 (Sv39模式关闭)
    void set_sv39_mode(bool sv39) { sv39_mode_ = sv39; }

    // 获取更新统计
    uint64_t get_update_ptwc_123_count() const { return update_ptwc_123_count_; }
    uint64_t get_update_ptwc_23_count() const { return update_ptwc_23_count_; }
    uint64_t get_update_ptwc_3_count() const { return update_ptwc_3_count_; }
    uint64_t get_update_none_count() const { return update_none_count_; }

    // 从地址中提取对应 Walker level 的累计段字段。
    static iova_t extract_addr_segment(iova_t addr, uint8_t level,
                                       bool addr_is_va, bool sv48,
                                       bool x4_mode);

private:
    // 3 个子表
    std::unique_ptr<WalkerSubCache> ptw_c1_;
    std::unique_ptr<WalkerSubCache> ptw_c2_;
    std::unique_ptr<WalkerSubCache> ptw_c3_;

    bool sv39_mode_ = false;

    // 更新类型统计
    uint64_t update_ptwc_123_count_ = 0;  // PTWC_1_2_3 更新次数
    uint64_t update_ptwc_23_count_ = 0;   // PTWC_2_3 更新次数
    uint64_t update_ptwc_3_count_ = 0;    // PTWC_3 更新次数
    uint64_t update_none_count_ = 0;      // NONE 跳过次数（避免冗余更新）

};

// Walker 子表: 继承 CacheBase 实现特定散列函数
class WalkerSubCache : public CacheBase<WalkerTag, WalkerData> {
public:
    SC_HAS_PROCESS(WalkerSubCache);

    WalkerSubCache(sc_module_name name, const CacheConfig& cfg,
                   StatsCollector& stats, const std::string& cache_name,
                   uint8_t level);
    bool lookup(gscid_t gscid, pscid_t pscid, iova_t va,
                bool va_pa_flag, bool stage_flag, bool sv48_flag,
                bool x4_mode_flag, WalkerData& out_data, sc_time& latency);
    struct UpdateResult {
        bool    updated = false;
        sc_time latency = SC_ZERO_TIME;
    };
    UpdateResult update_entry(const WalkerTag& tag, const WalkerData& data,
                              bool direct_write);
    uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                            bool has_gscid, bool has_pscid, bool has_iova,
                            CacheInvalidateMode mode, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid(gscid_t gscid, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                       sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const WalkerTag& tag) const override;

private:
    uint8_t level_;
};

} // namespace iommu

#endif // IOMMU_WALKER_CACHE_H
