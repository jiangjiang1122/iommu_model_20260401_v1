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
    uint32_t num_sets() const { return num_sets_; }

    // [失效] 延迟失效(LIB)查询旁路命中并丢弃失效数据的次数。
    // 仅在 LIB 非空时可能自增(冷路径), 用于量化验证延迟失效策略是否生效。
    uint64_t lazy_inval_drops() const { return lazy_inval_drops_; }

    // ============================================================
    // [失效][多RAM] 失效原子段与候选 set 计算
    //   失效指令由 pt_hash_thread 拆分为子失效分发到各 RAM worker,
    //   保证与 in-flight lookup/fill 在同一 RAM 内串行(原子段不可分割)。
    // ============================================================

    // 精准失效(PRECISE): 由 gscid/pscid/iova 直接算出唯一候选 set
    uint32_t precise_candidate_set(gscid_t gscid, pscid_t pscid,
                                   iova_t iova) const;

    // 小范围枚举失效(SCAN_RANGE): 枚举 gscid/pscid 在哈希中的 8 种 temp1,
    // 得到该 addr 对应的全部可能缓存索引; out_sets 容量需 >= 8,
    // 返回去重后的候选 set 数量
    uint32_t enum_candidate_sets(iova_t iova, uint32_t* out_sets) const;

    // set <-> RAM 映射 (global_set = ram_id * sets_per_ram + set_in_ram)
    uint32_t ram_of_set(uint32_t set) const {
        return (num_rams_ <= 1) ? 0 : (set / sets_per_ram_);
    }
    uint32_t ram_set_begin(uint32_t ram_id) const {
        return (num_rams_ <= 1) ? 0 : (ram_id * sets_per_ram_);
    }
    uint32_t ram_set_end(uint32_t ram_id) const {
        return (num_rams_ <= 1) ? num_sets_ : ((ram_id + 1) * sets_per_ram_);
    }

    // 单 set 失效原子段 (PRECISE / SCAN_RANGE 子失效)
    uint32_t invalidate_set_ram(uint32_t set, const CacheMessage& cmd,
                                sc_time& ram_latency);

    // 本 RAM set 区间扫描失效原子段 (SCAN / GLOBAL 子失效)
    uint32_t invalidate_ram_range(uint32_t ram_id, const CacheMessage& cmd,
                                  sc_time& ram_latency);

    // LIB 批量扫表原子段 (VN 回绕触发)
    uint32_t lazy_sweep_ram(uint32_t ram_id, uint8_t new_vn,
                            sc_time& ram_latency);

    // 失效谓词: 根据 IOTINVAL.VMA/GVMA 的 GV/PSCV/AV 与全局映射规则
    // 判定一条 cache line 是否属于本次失效范围
    bool line_matches_inval(const PTTag& tag, const PTData& data,
                            const CacheMessage& cmd) const;

    // [失效] 旧的单体失效接口(invalidate_vma/gvma/by_gscid/by_gscid_pscid/global)
    // 已删除: 它们绕过多RAM原子段与 LIB 语义, 全部由上述
    // invalidate_set_ram / invalidate_ram_range / lazy_sweep_ram 取代;
    // GLOBAL 模式通过 invalidate_ram_range(cmd.invalidate_mode==GLOBAL) 实现。

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

    // [失效] 将 raw hash 值映射为全局 set 号 (RAM 分组拆分逻辑)
    uint32_t set_from_raw(uint32_t raw) const;
    // [失效] 新哈希的 temp2 分量: temp2 = PN ^ (PN >> 22), PN = iova >> 12
    static uint32_t hash_temp2(iova_t iova);
    // [失效] 新哈希的 temp1 分量: temp1 = (gscid ^ pscid) & 0b111
    static uint32_t hash_temp1(gscid_t gscid, pscid_t pscid);
    // [失效] 由 temp1/temp2 合成 result: ((temp1 << (log2S-3)) ^ temp2) & (S-1)
    uint32_t hash_combine(uint32_t temp1, uint32_t temp2) const;

    // [失效] 插入时记录的全局 VN (LIB 未注入时为 0)
    uint8_t current_vn() const { return lib_ ? lib_->global_vn() : 0; }

    uint32_t num_rams_ = 1;       // RAM分组数(2的幂)
    uint32_t log2_num_rams_ = 0;  // log2(num_rams_)
    uint32_t sets_per_ram_ = 0;   // 每组RAM的set数
    uint32_t log2_num_sets_ = 0;  // [失效] log2(num_sets_), 新哈希移位量用
    bool     hash_v2_ = true;     // [失效] true=新哈希(inval_v2), false=legacy
    uint64_t lazy_inval_drops_ = 0;  // [失效] LIB 旁路丢弃失效CL的次数
};

} // namespace iommu

#endif // IOMMU_PT_CACHE_H
