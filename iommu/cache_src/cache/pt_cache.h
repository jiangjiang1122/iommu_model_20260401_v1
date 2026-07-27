#ifndef IOMMU_PT_CACHE_H
#define IOMMU_PT_CACHE_H

#include "cache/cache_base.h"

namespace iommu {

class PTCache : public CacheBase<PTTag, PTData> {
public:
    SC_HAS_PROCESS(PTCache);

    PTCache(sc_module_name name, const CacheConfig& cfg,
            StatsCollector& stats);

    ~PTCache() override = default;

    // PT Cache 特有接口
    bool lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                   TransStage stage, bool sv48, bool gstage_x4,
                   PTData& out_data, sc_time& latency);
    bool lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                   TransStage stage, PTData& out_data, sc_time& latency) {
        return lookup_pt(gscid, pscid, iova, stage, true, false, out_data, latency);
    }

    void fill_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
                 TransStage stage, const PTData& data, bool from_prefetch = false);

    // ============================================================
    // [多RAM] 多 RAM 方案接口: Hash 单元与 RAM 原子段分离
    //   - Hash 单元消耗 hash_latency_cycles, 由外部 pt_hash_thread 执行
    //   - RAM 原子段(lookup: read_set+compare; fill: read_set+compute+write)
    //     由对应 RAM worker 线程消耗, 同 RAM 串行/跨 RAM 并发
    //   - ram_id = raw_hash & (num_rams-1), set = ram_id*sets_per_ram + (raw>>log2)&(sets_per_ram-1)
    // ============================================================

    // 计算任务所属 RAM 组号 (无延时; hash仅依赖 gscid/pscid/iova)
    uint32_t compute_ram_id(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    // RAM 原子段查询: 纯功能访问(无 wait/无仲裁), ram_latency 返回原子段延时(不含hash)
    bool lookup_pt_ram(gscid_t gscid, pscid_t pscid, iova_t iova,
                       TransStage stage, bool sv48, bool gstage_x4,
                       PTData& out_data, sc_time& ram_latency);

    // RAM 原子段填充: 纯功能访问(无 wait/无仲裁), ram_latency 返回原子段延时(不含hash)
    void fill_pt_ram(gscid_t gscid, pscid_t pscid, iova_t iova,
                     TransStage stage, const PTData& data, bool from_prefetch,
                     sc_time& ram_latency);

    // Hash 单元单拍延时
    sc_time hash_stage_latency() const { return cycles_to_time(cfg_.hash_latency_cycles); }

    uint32_t num_rams() const { return num_rams_; }
    uint32_t ram_fifo_depth() const { return cfg_.ram_fifo_depth; }

    // 失效操作
    // IOTINVAL.VMA: 第一阶段页表失效
    uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                            bool has_gscid, bool has_pscid, bool has_iova,
                            CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                            sc_time* latency = nullptr);

    // IOTINVAL.GVMA: 第二阶段页表失效
    uint32_t invalidate_gvma(gscid_t gscid, iova_t gpa,
                             bool has_gscid, bool has_gpa,
                             CacheInvalidateMode mode = CacheInvalidateMode::SCAN,
                             sc_time* latency = nullptr);

    // 按 gscid/pscid 批量失效 (级联用)
    uint32_t invalidate_by_gscid(gscid_t gscid, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                       sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

protected:
    uint32_t hash_function(const PTTag& tag) const override;

private:
    // 将 iova 按页大小对齐
    static iova_t align_iova(iova_t iova, PageSize ps);

    // [多RAM] 未掩码的原始hash值(供 ram_id/set 拆分)
    uint32_t raw_hash(const PTTag& tag) const;
    // [多RAM] 构造 lookup/fill 共用的 tag
    static PTTag make_tag(gscid_t gscid, pscid_t pscid, iova_t iova,
                          TransStage stage, bool sv48, bool gstage_x4);

    uint32_t num_rams_ = 1;       // RAM分组数(2的幂)
    uint32_t log2_num_rams_ = 0;  // log2(num_rams_)
    uint32_t sets_per_ram_ = 0;   // 每组RAM的set数
};

} // namespace iommu

#endif // IOMMU_PT_CACHE_H
