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

    // [S2] S2 Cache 查询: 用GPA查询is_s2=1的cacheline (G-stage显式第二阶段)
    // gpa: 第一阶段翻译完成的GPA，用作查询key
    // x4_mode_flag: G-stage使用Sv48x4/Sv39x4，通常为true
    bool lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                   bool sv48_flag, bool x4_mode_flag,
                   WalkerData& out_data, uint8_t& hit_level, sc_time& latency);

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

    // [S2] S2 Cache 更新: 更新is_s2=1的cacheline
    UpdateResult update_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
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

    // [S2] S2 Cache统计接口
    uint64_t get_s2_lookup_count() const { return s2_lookup_count_; }
    uint64_t get_s2_hit_c3_count() const { return s2_hit_c3_count_; }
    uint64_t get_s2_hit_c2_count() const { return s2_hit_c2_count_; }
    uint64_t get_s2_hit_c1_count() const { return s2_hit_c1_count_; }
    uint64_t get_s2_miss_count() const { return s2_miss_count_; }
    // [大页] S2端到端leaf命中数(包含在对应级hit计数中, 此处单独计数)
    uint64_t get_s2_leaf_hit_count() const { return s2_leaf_hit_count_; }

    // [VS] VS-stage Walker Cache统计接口
    uint64_t get_vs_lookup_count() const { return vs_lookup_count_; }
    uint64_t get_vs_hit_c3_count() const { return vs_hit_c3_count_; }
    uint64_t get_vs_hit_c2_count() const { return vs_hit_c2_count_; }
    uint64_t get_vs_hit_c1_count() const { return vs_hit_c1_count_; }
    uint64_t get_vs_miss_count() const { return vs_miss_count_; }
    // [大页] VS端到端leaf命中数
    uint64_t get_vs_leaf_hit_count() const { return vs_leaf_hit_count_; }

    // ============================================================
    // [多RAM] 三级子表独立分RAM方案接口 (供 cache_subsystem 的
    // walker_hash_thread / walker_ram_worker_thread / walker_join_thread 使用)
    //   - 每个子表按本级 raw hash 低 log2(num_rams) 位选 RAM;
    //   - 子操作原子段(lookup: read_set+compare; update: read_set+compute+write)
    //     由 RAM worker 消耗, 同 RAM 串行/跨 RAM 并发; hash 1拍由 Hash 线程消耗
    // ============================================================

    // 计算某级子查询/更新的目标 RAM 组号
    uint32_t compute_ram_id(uint8_t level, gscid_t gscid, pscid_t pscid,
                            iova_t addr, bool va_pa_flag, bool stage_flag,
                            bool sv48_flag, bool x4_mode_flag) const;

    // 单级子表原子段查询 (无wait, ram_latency返回原子段延时, 不含hash)
    bool lookup_level_ram(uint8_t level, gscid_t gscid, pscid_t pscid,
                          iova_t va, bool va_pa_flag, bool stage_flag,
                          bool sv48_flag, bool x4_mode_flag,
                          WalkerData& out_data, sc_time& ram_latency);

    // 单级子表原子段更新 (无wait; level=1时direct_write, 与旧update路径一致)
    bool update_level_ram(uint8_t level, gscid_t gscid, pscid_t pscid,
                          iova_t va, const WalkerData& data,
                          sc_time& ram_latency);

    // join仲裁后记录VS lookup结果统计 (hit_level: 0=miss,1/2/3; is_leaf: 端到端leaf命中)
    void record_vs_lookup_result(uint8_t hit_level, bool is_leaf = false);
    // hash线程拆分update时记录更新类型统计
    void record_update_kind(WalkerUpdateKind kind);

    bool sv39_mode() const { return sv39_mode_; }
    // 三子表统一的RAM分组数(构造时断言三者一致)
    uint32_t num_rams() const { return num_rams_; }
    uint32_t ram_fifo_depth() const { return ram_fifo_depth_; }

    // ============================================================
    // [失效][多RAM] 失效子操作接口: 与 PT Cache 对齐
    //   失效指令由 walker_hash_thread 拆分为 (级 x RAM) 子失效,
    //   经高优先通道分发到对应 worker, 保证与 in-flight 子查询串行。
    // ============================================================

    // 将 LIB 注入三级子表(与 PT Cache 共享同一个 LIB)
    void set_lazy_invalid_buffer(const LazyInvalidBuffer* lib);

    // 本级子表的候选 set 枚举 / set->RAM 映射
    uint32_t enum_candidate_sets(uint8_t level, iova_t addr, bool addr_is_va,
                                 bool sv48, bool x4_mode,
                                 uint32_t* out_sets) const;
    uint32_t ram_of_set(uint8_t level, uint32_t set) const;

    // 单 set / 本RAM 区间 / LIB 批量扫表 失效原子段
    uint32_t invalidate_set_ram(uint8_t level, uint32_t set,
                                const CacheMessage& cmd, sc_time& ram_latency);
    uint32_t invalidate_ram_range(uint8_t level, uint32_t ram_id,
                                  const CacheMessage& cmd,
                                  sc_time& ram_latency);
    uint32_t lazy_sweep_ram(uint8_t level, uint32_t ram_id, uint8_t new_vn,
                            sc_time& ram_latency);

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

    // [S2] S2 Cache统计
    uint64_t s2_lookup_count_ = 0;
    uint64_t s2_hit_c3_count_ = 0;
    uint64_t s2_hit_c2_count_ = 0;
    uint64_t s2_hit_c1_count_ = 0;
    uint64_t s2_miss_count_ = 0;
    uint64_t s2_leaf_hit_count_ = 0;   // [大页] S2端到端leaf命中数

    // [VS] VS-stage Walker Cache统计
    uint64_t vs_lookup_count_ = 0;
    uint64_t vs_hit_c3_count_ = 0;
    uint64_t vs_hit_c2_count_ = 0;
    uint64_t vs_hit_c1_count_ = 0;
    uint64_t vs_miss_count_ = 0;
    uint64_t vs_leaf_hit_count_ = 0;   // [大页] VS端到端leaf命中数

    // [多RAM] 三子表统一的RAM分组参数(来自cfg, 构造时校验一致)
    uint32_t num_rams_ = 1;
    uint32_t ram_fifo_depth_ = 8;

    // 根据level选择子表
    WalkerSubCache* sub_cache(uint8_t level) const;

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
    // [S2] S2 Cache子表查询: 构造tag时设is_s2=true
    bool lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                   bool sv48_flag, bool x4_mode_flag,
                   WalkerData& out_data, sc_time& latency);
    struct UpdateResult {
        bool    updated = false;
        sc_time latency = SC_ZERO_TIME;
    };
    // [S2] S2 Cache子表更新: 使用独立的s2_cache_array_
    UpdateResult update_entry_s2(const WalkerTag& tag, const WalkerData& data);
    // [S2] S2 Cache失效
    uint32_t invalidate_s2_by_gscid(gscid_t gscid);
    uint32_t invalidate_s2_global();
    UpdateResult update_entry(const WalkerTag& tag, const WalkerData& data,
                              bool direct_write);

    // ============================================================
    // [多RAM] 原子段接口: 纯功能访问(无wait/无仲裁), 延时由RAM worker消耗
    //   ram_id = raw_hash & (num_rams-1)
    //   set    = ram_id * sets_per_ram + ((raw >> log2(num_rams)) & (sets_per_ram-1))
    // ============================================================
    void configure_multi_ram(uint32_t num_rams);
    uint32_t compute_ram_id(const WalkerTag& tag) const;
    // 原子段查询: ram_latency = read_set + compare (不含hash)
    bool lookup_ram(const WalkerTag& tag, WalkerData& out_data,
                    sc_time& ram_latency);
    // 原子段更新: ram_latency = read_set + compute_index + write_way (不含hash);
    //   direct_write路径仅write_way。valid=0时跳过(防缓存污染), 返回false
    bool update_entry_ram(const WalkerTag& tag, const WalkerData& data,
                          bool direct_write, sc_time& ram_latency);

    uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                            bool has_gscid, bool has_pscid, bool has_iova,
                            CacheInvalidateMode mode, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid(gscid_t gscid, sc_time* latency = nullptr);
    uint32_t invalidate_by_gscid_pscid(gscid_t gscid, pscid_t pscid,
                                       sc_time* latency = nullptr);
    uint32_t invalidate_global(sc_time* latency = nullptr);

    // ============================================================
    // [失效][多RAM] 失效原子段与候选 set 计算 (与 PT Cache 同构)
    //   小范围枚举: 同样枚举关联于 gscid/pscid 的 8 个 temp1 值,
    //   与本级 addr 段异或后得到 8 个 Cache set 索引。
    // ============================================================
    uint32_t num_sets_count() const { return num_sets_; }
    uint32_t ram_of_set(uint32_t set) const {
        return (num_rams_ <= 1) ? 0 : (set / sets_per_ram_);
    }
    uint32_t ram_set_begin(uint32_t ram_id) const {
        return (num_rams_ <= 1) ? 0 : (ram_id * sets_per_ram_);
    }
    uint32_t ram_set_end(uint32_t ram_id) const {
        return (num_rams_ <= 1) ? num_sets_ : ((ram_id + 1) * sets_per_ram_);
    }

    // 本级子表的候选 set 枚举 (返回去重后的数量, out_sets 容量需 >= 8)
    uint32_t enum_candidate_sets(iova_t addr, bool addr_is_va, bool sv48,
                                 bool x4_mode, uint32_t* out_sets) const;

    // 失效谓词: 本级 cache line 是否属于本次失效范围
    bool line_matches_inval(const WalkerTag& tag, const CacheMessage& cmd) const;

    // 单 set / 本RAM 区间 / LIB 批量扫表 失效原子段 (均含 S2 阵列)
    uint32_t invalidate_set_ram(uint32_t set, const CacheMessage& cmd,
                                sc_time& ram_latency);
    uint32_t invalidate_ram_range(uint32_t ram_id, const CacheMessage& cmd,
                                  sc_time& ram_latency);
    uint32_t lazy_sweep_ram(uint32_t ram_id, uint8_t new_vn,
                            sc_time& ram_latency);

protected:
    uint32_t hash_function(const WalkerTag& tag) const override;

private:
    uint8_t level_;

    // [多RAM] RAM分组参数(参照PTCache: 低位选RAM+连续set区间)
    uint32_t num_rams_ = 1;
    uint32_t log2_num_rams_ = 0;
    uint32_t sets_per_ram_ = 0;
    uint32_t log2_num_sets_ = 0;  // [失效] log2(num_sets_), 新哈希移位量用
    bool     hash_v2_ = true;     // [失效] true=新哈希(支持addr枚举), false=legacy

    // [多RAM] 未掩码原始hash
    uint32_t raw_hash(const WalkerTag& tag) const;
    // [失效] raw hash -> 全局 set 号 (RAM 分组拆分)
    uint32_t set_from_raw(uint32_t raw) const;
    // [失效] 新哈希分量 (与 PT Cache 同构)
    static uint32_t hash_temp1(gscid_t gscid, pscid_t pscid);
    static uint32_t hash_temp2(iova_t va_segment);
    uint32_t hash_combine(uint32_t temp1, uint32_t temp2) const;

    // [失效] 插入时记录的全局 VN (LIB 未注入时为 0)
    uint8_t current_vn() const { return lib_ ? lib_->global_vn() : 0; }

    // [失效] S2 阵列的区间失效 / LIB 批量扫表 (S2 与普通阵列物理隔离, 单独处理)
    uint32_t invalidate_s2_range(uint32_t set_begin, uint32_t set_end,
                                 const CacheMessage& cmd);
    uint32_t lazy_sweep_s2_range(uint32_t set_begin, uint32_t set_end,
                                 uint8_t new_vn);

    // [S2] 独立的S2 Cache存储阵列，与普通Walker Cache物理隔离
    std::vector<std::vector<CacheLine<WalkerTag, WalkerData>>> s2_cache_array_;

    // [S2] S2阵列初始化和失效辅助方法
    void init_s2_cache_array();
    uint32_t invalidate_s2_entries(
        std::function<bool(const WalkerTag&)> predicate);
};

} // namespace iommu

#endif // IOMMU_WALKER_CACHE_H
