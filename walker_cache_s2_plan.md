# Walker Cache S2 Cache 添加方案

## 背景

当前两阶段地址翻译中，VS-stage walk完成后获得GPA，进入GS_EXPLICIT阶段需要4次DDR read（Sv48x4: level 3->2->1->0）。S2 Cache缓存GS_EXPLICIT的中间walk结果，后续相同GPA区域的翻译可直接命中跳过DDR访问。

## 核心设计

- 在现有Walker Cache中新增3个S2子表（ptw_s2_c1/c2/c3），与现有3个子表（ptw_c1/c2/c3）独立
- WalkerTag添加`is_s2`标志位区分S2和常规entry
- S2子表以GPA为key（通过extract_addr_segment提取GPA段），常规子表以IOVA为key
- 全局开关`walker_cache_s2_enabled`控制功能启停

---

## Task 1: 修改WalkerTag和walker_reserved_t结构

**文件**: `iommu/cache_src/cache/cache_line.h` (WalkerTag)
**文件**: `iommu/cache_src/common/types.h` (walker_reserved_t)

1. WalkerTag添加`bool is_s2 = false`字段
2. 修改`operator==`增加`is_s2`比较
3. walker_reserved_t添加`uint8_t is_s2:1`标志位（从reserved的3bit中取1bit）

```cpp
// cache_line.h - WalkerTag
struct WalkerTag {
    // ... 现有字段 ...
    bool     is_s2        = false;  // S2 Cache标志
    bool operator==(const WalkerTag& o) const {
        return ... && is_s2 == o.is_s2;
    }
};

// types.h - walker_reserved_t
union walker_reserved_t {
    struct {
        uint8_t valid:1;
        uint8_t va_pa_flag:1;
        uint8_t stage_flag:1;
        uint8_t sv48_flag:1;
        uint8_t x4_mode_flag:1;
        uint8_t is_s2:1;           // 新增
        uint8_t reserved:2;        // 3->2
    };
    uint8_t raw = 0;
};
```

---

## Task 2: WalkerCache类添加S2子表

**文件**: `iommu/cache_src/cache/walker_cache.h`

1. 新增3个S2子表成员：`ptw_s2_c1_`, `ptw_s2_c2_`, `ptw_s2_c3_`
2. 新增S2 lookup/fill方法
3. 新增S2统计计数器
4. 构造函数增加S2子表配置参数

```cpp
// 新增方法
bool lookup_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
               bool sv48_flag, bool x4_mode_flag,
               WalkerData& out_data, uint8_t& hit_level, sc_time& latency);
void fill_s2(gscid_t gscid, pscid_t pscid, iova_t gpa,
             uint8_t level, const WalkerData& data,
             bool sv48_flag, bool x4_mode_flag);

// 新增成员
std::unique_ptr<WalkerSubCache> ptw_s2_c1_;
std::unique_ptr<WalkerSubCache> ptw_s2_c2_;
std::unique_ptr<WalkerSubCache> ptw_s2_c3_;

// S2统计
uint64_t s2_hit_c1_ = 0, s2_hit_c2_ = 0, s2_hit_c3_ = 0, s2_miss_ = 0;
```

---

## Task 3: WalkerCache S2方法实现

**文件**: `iommu/cache_src/cache/walker_cache.cpp`

### 3.1 lookup_s2实现
- 用GPA调用extract_addr_segment提取GPA段（addr_is_va=false，因为GPA不是VA）
- 按C3>C2>C1优先级查询S2子表
- S2子表的tag中is_s2=true，stage_flag=false（非两阶段标志，因为这是纯G-stage）
- 返回hit_level和out_data

### 3.2 fill_s2实现
- 构造WalkerTag：is_s2=true, va_segment=extract_addr_segment(gpa, level, false, sv48, x4)
- 构造WalkerData：reserved.is_s2=1
- 按level填入对应S2子表

### 3.3 构造函数
- 创建3个S2子表，配置与常规子表相同（num_sets, num_ways等）
- 统计名称: "walker_s2_c1", "walker_s2_c2", "walker_s2_c3"

### 3.4 invalidation
- invalidate_global/invalidate_vma中同时清除S2子表

---

## Task 4: PTW集成 - S2 Cache查询

**文件**: `iommu/iommu_perf_model/iommu_perf_ptw.cc`

### 4.1 GS_EXPLICIT启动时查询S2 Cache

在所有调用`init_gstage_walk(task, gpa)`后设置`walk_phase = PTW_GS_EXPLICIT`的位置，插入S2 Cache查询逻辑：

**关键位置**（共3处）：
1. L703: VS_WALK leaf → GS_EXPLICIT（主任务）
2. L93-94: Bare mode → GS_EXPLICIT
3. L1011: VS_PTE_READ → GS_EXPLICIT（预取任务） 

```cpp
// 在init_gstage_walk之后、发送DDR请求之前
init_gstage_walk(task, task->gpa);
task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;

// [S2 Cache] 查询S2 Cache
if (walker_cache_s2_enabled && task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
    WalkerData s2_data;
    uint8_t s2_hit_level;
    sc_time s2_lat;
    bool s2_hit = cache_sub.walker_cache().lookup_s2(
        task->GSCID, task->PSCID, task->gpa,
        (task->iohgatp.MODE == IOHGATP_Sv48x4),
        (task->iohgatp.MODE == IOHGATP_Sv39x4 || task->iohgatp.MODE == IOHGATP_Sv48x4), 
        s2_data, s2_hit_level, s2_lat);
    
    if (s2_hit) {
        // 根据hit_level调整gs_level和base_addr，跳过已缓存的层级
        // hit_level=3: 从level 0开始（只需1次DDR read）
        // hit_level=2: 从level 1开始（需2次DDR read）
        // hit_level=1: 从level 2开始（需3次DDR read）
        task->walk_ctx.gs_level = 3 - s2_hit_level;
        task->walk_ctx.gs_base_addr = s2_data.next_ppn * PAGESIZE;
        uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
            task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
        task->walk_ctx.read_addr = gs_pte_addr;
        task->walk_ctx.s2_cache_hit_level = s2_hit_level;  // 记录用于后续update
    } else {
        task->walk_ctx.s2_cache_hit_level = 0;  // miss
    }
}
```

需要在walk_ctx中添加字段：
- `uint8_t s2_cache_hit_level = 0`：记录S2 Cache命中级别
- `uint64_t s2_gpa = 0`：记录GPA用于后续fill
- `bool s2_cache_applicable = false`：标记本次GS_EXPLICIT是否适用S2

---

## Task 5: PTW集成 - S2 Cache更新（立即填充方案）

**文件**: `iommu/iommu_perf_model/iommu_perf_ptw.cc`

### 核心思路

在`ptw_walk_thread`的`PTW_GS_EXPLICIT` case的**non-leaf分支**中，每读取一个non-leaf PTE，**立即**将其中间结果（PPN）填充到对应级别的S2 Cache子表中。不再延迟到walk_complete_handler统一填充。

### 5.1 在PTW_GS_EXPLICIT non-leaf分支中立即fill S2 Cache

在`PTW_GS_EXPLICIT` case的non-leaf分支中（gs_pte.R==0 && gs_pte.X==0），在计算`gs_next`之后、发送DDR请求之前，插入S2 Cache fill：

```cpp
// G-stage non-leaf: continue walk
// ... gs_level--, gs_base_addr计算 ...

// [S2 Cache] 立即填充当前层级的中间结果
if (walker_cache_s2_enabled && task->walk_ctx.s2_cache_applicable) {
    WalkerData s2_data;
    s2_data.next_ppn = gs_pte.PPN;
    s2_data.reserved.valid = 1;
    s2_data.reserved.is_s2 = 1;
    // gs_level已经递减，当前读取的PTE所在层级 = gs_level + 1
    // gs_level=2 → 刚读完level3的PTE → fill C3
    // gs_level=1 → 刚读完level2的PTE → fill C2
    // gs_level=0 → 刚读完level1的PTE → fill C1
    uint8_t s2_fill_level = task->walk_ctx.gs_level + 1;
    cache_sub.walker_cache().fill_s2(
        task->GSCID, task->PSCID, task->gpa,
        s2_fill_level, s2_data,
        (task->iohgatp.MODE == IOHGATP_Sv48x4),
        (task->iohgatp.MODE == IOHGATP_Sv39x4 || task->iohgatp.MODE == IOHGATP_Sv48x4));
}

need_next_ddr = true;
```

### 5.2 优势

- **无需在walk_ctx中保存中间PPN**：不需要s2_cache_ppn_levelX字段
- **无需在walk_complete_handler中添加fill逻辑**：简化完成路径
- **语义清晰**：每级non-leaf PTE读取后立即缓存，与walk过程同步

---

## Task 6: 全局开关和参数

**文件**: `iommu/iommu_perf_model/iommu_perf_params.hh` 或 `iommu/iommu_top.cc`

```cpp
// 编译期开关
#ifndef WALKER_CACHE_S2_ENABLED
#define WALKER_CACHE_S2_ENABLED 0
#endif

// 运行时变量
bool walker_cache_s2_enabled = WALKER_CACHE_S2_ENABLED;
```

Makefile中添加编译选项：
```makefile
# 启用S2 Cache
# CXXFLAGS += -DWALKER_CACHE_S2_ENABLED=1
```

---

## Task 7: 统计输出

**文件**: `iommu/iommu_top.cc`

在性能报告末尾添加S2 Cache统计：

```
========== Walker Cache S2 Statistics ==========
  S2 Cache Enabled:    YES/NO
  S2 Lookup:           XXX
  S2 Hit C1:           XXX
  S2 Hit C2:           XXX
  S2 Hit C3:           XXX
  S2 Miss:             XXX
  S2 Hit Rate:         XX.X%
  DDR reads saved:     XXX
================================================
```

---

## Task 8: 编译验证和回归测试

1. 关闭S2 Cache（默认）：编译并运行4场景，确认无影响
2. 开启S2 Cache：编译并运行场景4（rand4k_twostage），验证功能正确性和性能提升
3. 开启S2 Cache：运行场景3（seq128k_twostage），验证功能正确性和性能提升

---

## 影响范围分析

| 场景 | 是否受影响 | 原因 |
|------|-----------|------|
| 场景1: seq128k_singlestage | 否 | 无两阶段翻译，不触发GS_EXPLICIT |
| 场景2: rand4k_singlestage | 否 | 同上 |
| 场景3: seq128k_twostage | 优化 | 顺序访问GPA区域高度相似，S2命中率预期很高，DDR reads显著减少 |
| 场景4: rand4k_twostage | 优化 | 随机访问也可能命中S2 Cache，减少GS_EXPLICIT的DDR reads |

## 涉及文件清单

| 文件 | 修改类型 |
|------|---------|
| `cache_src/cache/cache_line.h` | WalkerTag添加is_s2 |
| `cache_src/common/types.h` | walker_reserved_t添加is_s2 |
| `cache_src/cache/walker_cache.h` | 添加S2子表、方法声明 |
| `cache_src/cache/walker_cache.cpp` | S2 lookup/fill实现 |
| `iommu_perf_model/iommu_perf_ptw.cc` | GS_EXPLICIT查询/更新S2 |
| `iommu_perf_model/iommu_perf_params.hh` | 添加开关宏 |
| `iommu_top.cc` | 统计输出 |
| `Makefile` / `build_wsl.sh` | 添加编译选项 |
