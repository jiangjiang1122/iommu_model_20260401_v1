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

    // NEW: PT Cache去重+预取接口（V3.0: tail_index已移至Buffer entry）
    // 插入占位Cache Line
    // 返回值: true=插入成功, false=替换失败(Cache full且所有CL都是is_req=1)
    bool insert_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova,
                           TransStage stage, bool sv48, bool gstage_x4,
                           uint16_t head_index, bool is_req = true,
                           sc_time* latency = nullptr);
    
    // 批量更新占位CL为常规CL
    void batch_update_placeholders(gscid_t gscid, pscid_t pscid,
                                   const std::vector<std::pair<iova_t, PTData>>& updates,
                                   TransStage stage, bool sv48, bool gstage_x4);
    
    // [V3.0] 更新占位CL的head_index和is_req（不再包含tail_index）
    // 用于分支3: 预取占位CL首次HIT时,将is_req=0更新为is_req=1
    bool update_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova,
                           TransStage stage, bool sv48, bool gstage_x4,
                           uint16_t head_index, bool is_req);

    // [OPT] 直接更新占位CL，跳过内部冗余lookup
    // 调用方已通过lookup_pt获取data，无需在update_placeholder内再次lookup
    // data: 调用方lookup_pt返回的占位CL数据（已验证is_ph=1, is_req=0）
    void update_placeholder_with_data(gscid_t gscid, pscid_t pscid, iova_t iova,
                                      TransStage stage, bool sv48, bool gstage_x4,
                                      PTData data, uint16_t head_index, bool is_req);

protected:
    uint32_t hash_function(const PTTag& tag) const override;

private:
    // 将 iova 按页大小对齐
    static iova_t align_iova(iova_t iova, PageSize ps);
};

} // namespace iommu

#endif // IOMMU_PT_CACHE_H
