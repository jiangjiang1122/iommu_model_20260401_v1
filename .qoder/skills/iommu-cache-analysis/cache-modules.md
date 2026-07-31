# IOMMU Cache 模块参考

## 模块概览

| 模块 | 源文件 | 头文件 | 配置 | 说明 |
|-----|-------|-------|-----|-----|
| PT Cache | `pt_cache.cpp` | `pt_cache.h` | `default_config.json` | 页表缓存，1024组×8路 |
| DC Cache | `dc_cache.cpp` | `dc_cache.h` | `default_config.json` | 设备上下文缓存，64组×4路 |
| PC Cache | `pc_cache.cpp` | `pc_cache.h` | `default_config.json` | 进程上下文缓存，64组×4路 |
| Walker Cache | `walker_cache.cpp` | `walker_cache.h` | `default_config.json` | 页表遍历缓存（C1/C2/C3） |
| MSIPT Cache | `msipt_cache.cpp` | `msipt_cache.h` | `default_config.json` | MSI页表缓存，64组×4路 |
| Dedup Cache | `dedup_cache.cpp` | `dedup_cache.h` | - | 去重缓存（V3.0），1024组×8路 |

## 基类架构

所有 Cache 模块继承自 `CacheBase<TagT, DataT>` 模板类：

```cpp
// cache_base.h
template <typename TagT, typename DataT>
class CacheBase : public sc_module {
protected:
    // RAM 端口互斥
    bool ram_port_busy_ = false;
    
    // 仲裁机制
    template <typename Fn>
    auto arbitrate_ram_access(CacheOpType op, Fn&& fn);
    
    // 核心操作
    bool lookup(const TagT& tag, DataT& out_data, sc_time& latency);
    void fill(const TagT& tag, const DataT& data, bool from_prefetch);
};
```

## 关键接口

### PT Cache

```cpp
// 查询
bool lookup_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
               TransStage stage, bool sv48, bool gstage_x4,
               PTData& out_data, sc_time& latency);

// 填充
void fill_pt(gscid_t gscid, pscid_t pscid, iova_t iova,
             TransStage stage, const PTData& data, bool from_prefetch);

// 失效
uint32_t invalidate_vma(gscid_t gscid, pscid_t pscid, iova_t iova,
                        bool has_gscid, bool has_pscid, bool has_iova,
                        CacheInvalidateMode mode, sc_time* latency);
```

### Walker Cache

```cpp
// 查询（返回 hit level）
bool lookup(gscid_t gscid, pscid_t pscid, iova_t va,
            WalkerData& out_data, uint8_t& hit_level, sc_time& latency);

// S2 Cache 查询
bool lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
               WalkerData& out_data, uint8_t& hit_level, sc_time& latency);

// 更新
UpdateResult update(gscid_t gscid, pscid_t pscid, iova_t va,
                    WalkerUpdateKind kind,
                    const WalkerData& ptwc1_data,
                    const WalkerData& ptwc2_data,
                    const WalkerData& ptwc3_data);
```

### Dedup Cache

```cpp
// 查询（返回占位符类型）
DedupCacheLine* lookup(gscid_t gscid, pscid_t pscid, iova_t iova);

// 插入主占位符
bool insert_main_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova);

// 插入预取占位符
bool insert_prefetch_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova);

// 清除占位符
void clear_placeholder(gscid_t gscid, pscid_t pscid, iova_t iova);
```

## 调度线程

| 线程 | 文件 | 调度算法 | FIFO 队列 |
|-----|-----|---------|----------|
| `pt_scheduler_thread` | `cache_subsystem.cpp` | 乒乓调度 | `pt_request_fifo`, `pt_update_fifo` |
| `dedup_scheduler_thread` | `cache_subsystem.cpp` | 乒乓调度 | `dedup_request_fifo`, `dedup_update_fifo` |
| `walker_scheduler_thread` | `cache_subsystem.cpp` | 轮询调度 | `walker_request_fifo`, `walker_update_fifo` |

## 延时配置

```json
// default_config.json
{
  "cache_timing": {
    "arbiter_latency_cycles": 1,
    "hash_latency_cycles": 1,
    "read_set_latency_cycles": 2,
    "compare_latency_cycles": 1,
    "update_way_select_latency_cycles": 1,
    "fill_compute_index_hit_cycles": 1,
    "fill_compute_index_invalid_cycles": 2,
    "fill_compute_index_replacement_cycles": 4,
    "write_way_latency_cycles": 1,
    "invalidation_compare_per_way_cycles": 1
  },
  "pt_cache": {
    "num_sets": 1024,
    "num_ways": 8,
    "replacement": "plru"
  }
}
```

## 原子阶段约束

所有 Cache 操作通过 `arbitrate_ram_access()` 实现互斥：

```cpp
// 原子阶段 = RAM 端口占用期间
arbitrate_ram_access(CacheOpType::LOOKUP, [&]() {
    // 1. hash 计算
    // 2. read set
    // 3. compare / fill
    // 4. write way (if fill)
});
```

**关键约束**：
- 同一时刻只有一个 LOOKUP/FILL/INVALIDATE 可以访问 RAM
- 调度线程单线程执行，天然串行
- FIFO 阻塞写入实现流控
