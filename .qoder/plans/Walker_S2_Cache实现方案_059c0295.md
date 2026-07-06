# Walker S2 Cache 实现方案

## 核心思路

在现有Walker Cache三级结构(C1/C2/C3)基础上，复用相同的物理存储，通过WalkerTag中添加`is_s2`标志位区分"普通Walker Cache"和"S2 Walker Cache"条目。S2 Cache专门缓存两阶段翻译中**显式第二阶段**(GS_EXPLICIT, GPA->SPA)的中间walk结果。

**关键约束**：
- 仅影响 `en_1S=1 && en_2S=1`（两阶段翻译）场景中的 GS_EXPLICIT 阶段
- 不影响单阶段翻译(STAGE1_ONLY)或独立第二阶段(STAGE2_ONLY)
- 总开关 `PTW_WALKER_S2_CACHE_ENABLED`，关闭时为现有基线状态

---

## Task 1: 数据结构扩展

### 1.1 WalkerTag 添加 is_s2 字段
**文件**: `iommu/cache_src/cache/cache_line.h` (L47-63)

```cpp
struct WalkerTag {
    gscid_t  gscid        = 0;
    pscid_t  pscid        = 0;
    iova_t   va_segment   = 0;
    uint8_t  level        = 0;
    bool     va_pa_flag   = true;
    bool     stage_flag   = true;
    bool     sv48_flag    = true;
    bool     x4_mode_flag = false;
    bool     is_s2        = false;  // [NEW] S2 Cache标志位
    bool operator==(const WalkerTag& o) const {
        return gscid == o.gscid && pscid == o.pscid &&
               va_segment == o.va_segment && level == o.level &&
               va_pa_flag == o.va_pa_flag &&
               stage_flag == o.stage_flag &&
               sv48_flag == o.sv48_flag &&
               x4_mode_flag == o.x4_mode_flag &&
               is_s2 == o.is_s2;  // [NEW]
    }
};
```

### 1.2 walker_reserved_t 添加 is_s2 位
**文件**: `iommu/cache_src/common/types.h` (L424-434)

利用reserved中剩余3bit之一：
```cpp
union walker_reserved_t {
    struct {
        uint8_t valid:1;
        uint8_t va_pa_flag:1;
        uint8_t stage_flag:1;
        uint8_t sv48_flag:1;
        uint8_t x4_mode_flag:1;
        uint8_t is_s2:1;       // [NEW] S2 Cache标志
        uint8_t reserved:2;    // 从3减为2
    };
    uint8_t raw = 0;
};
```

### 1.3 辅助函数和 make_walker_data 扩展
**文件**: `iommu/cache_src/common/types.h` (L441-465)

添加：
```cpp
inline bool walker_is_s2(const WalkerData& data) { return data.reserved.is_s2 != 0; }
```

`make_walker_data` 增加 `is_s2` 参数（默认false）：
```cpp
inline WalkerData make_walker_data(ppn_t next_ppn,
                                   bool valid = true,
                                   bool is_va = true,
                                   bool is_stage1_and_2 = true,
                                   bool is_sv48 = true,
                                   bool is_x4_mode = false,
                                   bool is_s2 = false) {  // [NEW]
    WalkerData data;
    data.next_ppn = next_ppn;
    data.reserved.valid = valid ? 1U : 0U;
    data.reserved.va_pa_flag = is_va ? 1U : 0U;
    data.reserved.stage_flag = is_stage1_and_2 ? 1U : 0U;
    data.reserved.sv48_flag = is_sv48 ? 1U : 0U;
    data.reserved.x4_mode_flag = is_x4_mode ? 1U : 0U;
    data.reserved.is_s2 = is_s2 ? 1U : 0U;  // [NEW]
    return data;
}
```

### 1.4 iommu_task_t walk_context 添加S2相关字段
**文件**: `iommu/include/iommu_task.hh` (walk_context_t内)

```cpp
// S2 Walker Cache 命中层级 (0=全miss, 3=c3 hit, 2=c2 hit, 1=c1 hit)
uint8_t s2_walker_hit_level = 0;

// S2 Walker Cache 中间结果 (GS_EXPLICIT walk过程中保存)
struct {
    uint64_t s2_ppn_level2 = 0;  // G-stage Level 2 PPN (S2_C3)
    uint64_t s2_ppn_level1 = 0;  // G-stage Level 1 PPN (S2_C2)
    uint64_t s2_ppn_level0 = 0;  // G-stage Level 0 PPN (S2_C1)
    bool s2_valid_level2 = false;
    bool s2_valid_level1 = false;
    bool s2_valid_level0 = false;
} s2_walker_cache_entries;
```

### 1.5 CacheMessage 添加S2相关字段
**文件**: `iommu/cache_src/common/types.h` (CacheMessage struct)

```cpp
// S2 Walker Cache lookup/update标志
bool walker_is_s2_lookup = false;  // S2 Cache查询请求
```

---

## Task 2: 编译开关

**文件**: `iommu/iommu_perf_model/iommu_perf_params.hh`

```cpp
#ifndef TEST_CFG_WALKER_CACHE_S2_ENABLED
static const bool PTW_WALKER_S2_CACHE_ENABLED = true;  // 默认开启
#else
static const bool PTW_WALKER_S2_CACHE_ENABLED = TEST_CFG_WALKER_CACHE_S2_ENABLED;
#endif
```

**文件**: `iommu/iommu_perf_model/iommu_perf_params_t2.hh` 同步添加。

---

## Task 3: Walker Cache S2 查询/更新接口

### 3.1 WalkerCache类扩展
**文件**: `iommu/cache_src/cache/walker_cache.h`

添加S2专用接口（复用现有子表，通过tag中is_s2=1区分）：

```cpp
// S2 Cache 查询: 用GPA查询is_s2=1的cacheline
bool lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
               bool sv48_flag, bool x4_mode_flag,
               WalkerData& out_data, uint8_t& hit_level, sc_time& latency);

// S2 Cache 更新
UpdateResult update_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
                       WalkerUpdateKind kind,
                       const WalkerData& ptwc1_data,
                       const WalkerData& ptwc2_data,
                       const WalkerData& ptwc3_data);
```

### 3.2 WalkerCache S2 查询实现
**文件**: `iommu/cache_src/cache/walker_cache.cpp`

`lookup_s2` 与现有 `lookup` 逻辑基本一致，关键差异：
- 调用子表lookup时，构造tag时设 `is_s2 = true`
- `va_pa_flag = false` (GPA不是VA)
- `stage_flag = true` (两阶段)
- 用GPA的va_segment做hash（GPA位宽使用55:39，因为x4模式下GPA是56位）

`update_s2` 与现有 `update` 逻辑基本一致，关键差异：
- 构造tag时设 `is_s2 = true`
- 同样使用GPA地址

---

## Task 4: PTW性能模型中集成S2 Cache

### 4.1 S2 Cache 查询时机和位置
**文件**: `iommu/iommu_perf_model/iommu_perf_ptw.cc`

**查询点**：在两个地方进入 GS_EXPLICIT 阶段之前插入S2 Cache查询：

1. **主任务**：VS_WALK leaf完成后 (L698-717)，在调用 `init_gstage_walk` 之前，先查询S2 Cache
2. **预取任务**：PTW_VS_PTE_READ 完成后 (L1010-1020)，在调用 `init_gstage_walk` 之前，先查询S2 Cache

**查询逻辑**（伪代码）：
```cpp
if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
    && task->GV && task->iohgatp.MODE != IOHGATP_Bare
    && task->iosatp.MODE != IOSATP_Bare) {  // 仅两阶段翻译
    
    // 构造S2 Cache查询: 使用GPA作为key
    iommu::CacheMessage s2_req = task_to_s2_walker_request(task, task->gpa);
    cache_sub.walker_request_fifo.write(s2_req);
    iommu::CacheMessage s2_resp = cache_sub.walker_response_fifo.read();
    
    if (s2_resp.hit) {
        task->walk_ctx.s2_walker_hit_level = s2_resp.walker_level;
        // 根据命中层级设置GS walk起始level
        // hit_level=3 -> gs_level从0开始(只需1次DDR)
        // hit_level=2 -> gs_level从1开始(需2次DDR)
        // hit_level=1 -> gs_level从2开始(需3次DDR)
        uint8_t gs_start_level = GS_LEVELS - 1 - s2_resp.walker_level;
        // 用S2 Cache返回的next_ppn设置gs_base_addr
        task->walk_ctx.gs_level = gs_start_level;
        task->walk_ctx.gs_base_addr = s2_resp.walker_data.next_ppn * PAGESIZE;
        // 计算正确的read_addr
    } else {
        task->walk_ctx.s2_walker_hit_level = 0;
        // 正常init_gstage_walk，从最高级开始
    }
}
```

### 4.2 S2 Cache 中间结果保存
**文件**: `iommu/iommu_perf_model/iommu_perf_ptw.cc`

在 GS_EXPLICIT non-leaf 处理逻辑中 (L1109-1136)，保存中间walk结果到 `s2_walker_cache_entries`：

```cpp
// GS_EXPLICIT non-leaf: 保存中间结果
// gs_level 对应 S2 Cache level:
// gs_level=3 -> s2_ppn_level0 (S2_C1, tag=GPA[55:39])
// gs_level=2 -> s2_ppn_level1 (S2_C2, tag=GPA[55:30])
// gs_level=1 -> s2_ppn_level2 (S2_C3, tag=GPA[55:21])
if (task->walk_ctx.gs_level == 3) {
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level0 = gs_pte.PPN;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level0 = true;
} else if (task->walk_ctx.gs_level == 2) {
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level1 = gs_pte.PPN;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level1 = true;
} else if (task->walk_ctx.gs_level == 1) {
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level2 = gs_pte.PPN;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level2 = true;
}
```

### 4.3 S2 Cache 更新时机和逻辑
**文件**: `iommu/iommu_perf_model/iommu_perf_ptw.cc`

在 GS_EXPLICIT leaf 完成后（walk_complete），构造S2 Walker Update请求。

**转换函数**: `iommu/iommu_perf_model/iommu_task_cache_convert.cc`

新增 `task_to_s2_walker_request()` 和 `task_to_s2_walker_update()` 函数：

```cpp
iommu::CacheMessage task_to_s2_walker_request(iommu_task_t* task, uint64_t gpa) {
    iommu::CacheMessage req;
    req.msg_type = iommu::CacheMsgType::WALKER_LOOKUP;
    req.task_id = task->task_id;
    req.gscid = task->GSCID;
    req.pscid = task->PSCID;
    req.iova = gpa;  // 使用GPA而非IOVA
    req.walker_addr_is_va = false;  // GPA不是VA
    req.walker_from_two_stage = true;
    req.walker_sv48 = true;  // G-stage使用Sv48x4
    req.walker_x4_mode = true;  // x4模式
    req.walker_is_s2_lookup = true;  // 标识S2查询
    return req;
}
```

**更新逻辑**：根据 `s2_walker_hit_level` 决定更新哪些级别：
- hit_level=3 (C1+C2+C3全命中): 不更新 (NONE)
- hit_level=2 (C1+C2命中): 更新 S2_C3
- hit_level=1 (C1命中): 更新 S2_C2 + S2_C3
- hit_level=0 (全miss): 更新 S2_C1 + S2_C2 + S2_C3

### 4.4 CacheSubsystem S2 请求处理
**文件**: `iommu/cache_src/subsystem/cache_subsystem.cpp`

在 `execute_walker_request` 中判断 `req.walker_is_s2_lookup`，若为true则调用 `walker_cache_->lookup_s2()`。

在 `execute_walker_update_request` 中判断类似的S2标志，调用 `walker_cache_->update_s2()`。

---

## Task 5: 统计和日志

**文件**: `iommu/iommu_top.cc`

在统计输出中添加S2 Cache相关统计：
- S2 Cache 查询次数 / 命中次数 / 命中率
- S2 Cache 各级命中分布 (C1 hit / C2 hit / C3 hit / miss)
- S2 Cache DDR walk节省次数

**文件**: `iommu/cache_src/cache/walker_cache.h/.cpp`

WalkerCache类添加S2专用统计计数器：
```cpp
uint64_t s2_lookup_count_ = 0;
uint64_t s2_hit_c3_count_ = 0;
uint64_t s2_hit_c2_count_ = 0;
uint64_t s2_hit_c1_count_ = 0;
uint64_t s2_miss_count_ = 0;
```

---

## Task 6: 编译验证和测试

1. 编译验证：确保 `PTW_WALKER_S2_CACHE_ENABLED=0` 时与基线完全一致
2. 编译验证：`PTW_WALKER_S2_CACHE_ENABLED=1` 时编译通过
3. 功能测试：两阶段翻译场景(rand4k_ts / seq128k_ts)验证DDR读次数减少
4. 回归测试：单阶段场景不受影响

---

## 影响范围汇总

| 文件 | 修改内容 |
|------|---------|
| `cache_line.h` | WalkerTag + is_s2 字段 |
| `types.h` | walker_reserved_t + is_s2 bit, make_walker_data + is_s2 参数, CacheMessage + walker_is_s2_lookup |
| `walker_cache.h/cpp` | lookup_s2 / update_s2 接口实现 |
| `cache_subsystem.cpp` | execute_walker_request/update 中分支S2 |
| `iommu_perf_params.hh` | PTW_WALKER_S2_CACHE_ENABLED 开关 |
| `iommu_task.hh` | walk_context_t + s2_walker_hit_level, s2_walker_cache_entries |
| `iommu_task_cache_convert.cc` | task_to_s2_walker_request / task_to_s2_walker_update |
| `iommu_perf_ptw.cc` | GS_EXPLICIT 前插入S2查询, non-leaf保存中间结果, leaf后触发S2更新 |
| `iommu_top.cc` | S2统计输出 |
