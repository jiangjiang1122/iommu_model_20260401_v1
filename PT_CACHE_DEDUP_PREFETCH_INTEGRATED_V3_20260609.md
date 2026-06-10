# PT Cache 去重+预取 + PTW Burst预取 + Walker Cache 整合方案 v3.0

**文档版本**: V3.0  
**生成日期**: 2026-06-09  
**文档类型**: 架构设计 + 实施方案  
**变更说明**: 基于V2.0迭代，核心变更：将 `tail_index` 从 PT Cache cacheline 移至 Buffer entry，大幅减少 PT Cache 写入操作

---

## 一、方案概述

### 1.1 背景

基于V2.0方案迭代。V2.0中 `tail_index` 存储在 PT Cache cacheline 中，导致每次追加任务到链表尾部时都需要执行一次 PT Cache 写入（更新 `tail_index`）。V3.0将 `tail_index` 移至 Buffer entry 中，仅在链头位置有效，彻底消除追加任务时的 PT Cache 写入操作。

### 1.2 核心优化目标

1. **PT Cache去重+预取**: 占位CL机制，Buffer链表管理，**最小化PT Cache写入**
2. **PTW Burst预取**: 利用页表连续性，1次burst读取替代多次独立walk
3. **Walker Cache优化**: 主任务查询/更新WC，预取任务跳过WC
4. **DDR访问优化**: 从1971次降至24次 (降低98.8%)

### 1.3 适用范围

- 支持4KB标准页 (**当前仿真仅考虑4KB页，大页场景暂不考虑**)
- Sv39/Sv48地址翻译
- PT Cache和Walker Cache规划为整体

### 1.4 DC Cache说明

**重要**: DC Cache (Device Context Cache) **不修改**，不添加预取和去重等逻辑。

- DC Cache保持现有实现
- 仅PT Cache (Page Table Cache)实现去重+预取机制
- DC Cache不参与占位CL、Buffer链表、Burst预取等优化

### 1.5 V2.0 → V3.0 关键变更

| 变更项 | V2.0 | V3.0 | 影响 |
|--------|------|------|------|
| `tail_index` 位置 | PT Cache cacheline | Buffer entry（仅链头有效） | **减少PT Cache写入** |
| PT Cache Contents | `head_index` + `tail_index` + `is_req` + `is_ph` + `V` | `head_index` + `is_req` + `is_ph` + `V` | cacheline结构精简 |
| HIT is_req=1 操作 | 写Buffer + **写PT Cache**(更新tail_index) | **仅写Buffer** | 消除1次PT Cache写 |
| HIT is_req=0 操作 | 写Buffer + **写PT Cache**(更新head/tail/is_req) | 写Buffer + **写PT Cache**(仅更新head/is_req) | 减少PT Cache写字段 |
| 大页支持 | 不考虑 | 支持（大页leaf PTE写入Walker Cache，PT Cache刷新构造4KB级iova）**[当前仿真暂不实现]** | 扩展适用范围 |

---

## 一.5 配置参数统一定义

**重要**: 以下所有参数在实现代码时，必须统一定义到头文件中，便于配置和管理。

```cpp
// 文件: iommu/include/iommu_dedup_params.hh

#ifndef IOMMU_DEDUP_PARAMS_HH
#define IOMMU_DEDUP_PARAMS_HH

#include <cstdint>

// ============================================================
// Buffer配置参数
// ============================================================

/// Dedup Buffer容量 (最大任务链表entry数)
static constexpr uint32_t DEDUP_BUFFER_CAPACITY = 256;

/// Buffer满时的反压策略: true=阻塞等待, false=丢弃请求
static constexpr bool DEDUP_BUFFER_BACKPRESSURE = true;

/// Buffer链表尾标记 (0xFF=无下一项)
static constexpr uint8_t BUFFER_CHAIN_END = 0xFF;

// ============================================================
// PT Cache配置参数
// ============================================================

/// PT Cache占位CL的is_ph标志位位置
static constexpr uint32_t PT_RESERVED_IS_PH_BIT = 7;

/// PT Cache占位CL的is_req标志位位置
static constexpr uint32_t PT_RESERVED_IS_REQ_BIT = 17;

// ============================================================
// 预取配置参数
// ============================================================

/// 默认预取深度 (D值)
/// D=0: 预取功能关闭
/// D>0: 预取功能启用,预取深度为D
static constexpr uint32_t DEFAULT_PREFETCH_DEPTH = 8;

/// 预取是否默认启用
static constexpr bool PREFETCH_ENABLED_BY_DEFAULT = true;

/// Burst读取的最大PTE数 (防止跨页表边界)
static constexpr uint32_t MAX_BURST_PTE_COUNT = 64;

// ============================================================
// PTE有效性标记
// ============================================================

/// PTE有效标志 (V位)
static constexpr uint64_t PTE_VALID_MASK = 0x1;

/// PTE无效/错误标记 (用于ptw_response_t)
enum class PTEStatus : uint8_t {
    PTE_VALID = 0,          ///< 正常有效PTE
    PTE_INVALID = 1,        ///< 无效PTE (V=0)
    PTE_ERROR = 2,          ///< 错误PTE (权限错误等)
    PTE_RESERVED = 3        ///< 保留
};

// ============================================================
// 页大小配置
// ============================================================

/// 4KB页大小 (字节)
static constexpr uint64_t PAGE_SIZE_4KB = 0x1000;

/// 4KB页掩码
static constexpr uint64_t PAGE_MASK_4KB = 0xFFF;

/// 页表页大小 (4KB, 存储512个PTE)
static constexpr uint64_t PT_PAGE_SIZE = 0x1000;

/// 页表页掩码
static constexpr uint64_t PT_PAGE_MASK = 0xFFF;

// ============================================================
// Sv39地址翻译参数
// ============================================================

/// Sv39 VPN位数
static constexpr uint32_t SV39_VPN_BITS = 9;

/// Sv39 PPN位数
static constexpr uint32_t SV39_PPN_BITS = 26;

/// Sv39页表级数
static constexpr uint32_t SV39_LEVELS = 3;

/// PTE大小 (Sv39为8字节)
static constexpr uint32_t PTE_SIZE = 8;

#endif // IOMMU_DEDUP_PARAMS_HH
```

**参数说明**:

| 参数 | 值 | 说明 |
|------|-----|------|
| `DEDUP_BUFFER_CAPACITY` | 256 | Buffer容量，满时反压阻塞 |
| `DEFAULT_PREFETCH_DEPTH` | 8 | 默认预取深度D, D=0表示关闭 |
| `PREFETCH_ENABLED_BY_DEFAULT` | true | 预取是否默认启用 |
| `MAX_BURST_PTE_COUNT` | 64 | Burst读取最大PTE数 |
| `PAGE_SIZE_4KB` | 0x1000 | 4KB页大小 |
| `PTE_SIZE` | 8 | Sv39 PTE大小 (8字节) |

---

## 二、数据结构设计

### 2.1 PT Cache占位CL数据结构（V3.0精简版）

**核心变更**: `tail_index` 从 cacheline 中移除，Contents 仅保留 `head_index`、`is_req`、`is_ph`、`V`。

```
┌─────────────────────────────┬──────────────────────────────┐
│            TAGs             │           Contents           │
├──────┬──────┬──────┬────────┼──────────┬───────────────────
│ iova*│GSCID │PSCID │*Remain │head index│*Remain│Is_req:1│Is_ph:1│V:1│
└──────┴──────┴──────┴────────┴──────────┴───────────────────┘
  图. PT Cache 占位 Cache line 的数据结构 (V3.0)
```

```cpp
// 文件: iommu/cache_src/common/types.h

union pt_reserved_t {
    struct {
        uint32_t valid:1;           // Cache line有效标志 (V)
        uint32_t trans_type:2;      // 翻译类型 (0=Sv39, 1=Sv48, ...)
        uint32_t input_page_size:2; // 输入页大小
        uint32_t result_page_size:2;// 结果页大小
        uint32_t iova_is_va:1;      // IOVA是否为VA
        uint32_t sv48:1;            // 是否Sv48
        uint32_t gstage_x4:1;       // 第二阶段x4模式
        uint32_t is_ph:1;           // 占位标志 (1=占位CL, 0=常规CL)
        uint32_t head_index:8;      // Buffer链表头编号
        uint32_t is_req:1;          // 1=主任务(有实际请求), 0=预取占位
        uint32_t replacement_info:2;// 替换算法信息 (LRU位)
        uint32_t reserved:10;       // 保留 (原6bit,现10bit,因移除tail_index)
    };
    uint32_t raw = 0;
};
```

**关键字段说明**:
- `head_index`: Buffer链表头，首个请求任务的Buffer编号。传递给PTW模块时，传递此字段。
- `is_req`: 
  - `1`: 主任务占位CL (有实际请求等待翻译)
  - `0`: 预取占位CL (预分配，等待首个任务到达)
- **V3.0变更**: `tail_index` 已移至 Buffer entry，PT Cache cacheline 不再存储

### 2.2 Buffer表项数据结构（V3.0扩展版）

**核心变更**: 新增 `tail_index` 字段，仅在任务链头位置有效，其余 entry 该值置为全1 (0xFF)。

```
┌──────────────────────────────────────────────────────────┐
│                        Command                           │
├─────┬──────────────────────────────┬──────────┬──────────┬──────────┐
│V:1  │ TAGs: iova, GSCID, PSCID     │*Remaining│next index│tail index│
─────┴──────────────────────────────┴──────────┴──────────┴──────────┘
  图. 地址翻译部分去重模块 Buffer 数据结构 (V3.0)
```

```cpp
// 文件: iommu/cache_src/common/dedup_buffer.h

struct DedupBufferEntry {
    uint8_t  valid;          // 表项有效标志 (V)
    uint64_t iova;           // 原始IOVA (未对齐)
    uint8_t  next_index;     // 下一个Buffer编号 (0xFF=链表尾)
    uint8_t  tail_index;     // [V3.0新增] 任务链尾编号 (仅链头有效, 其余=0xFF)
    void*    task_ptr;       // 指向原始任务指针
};
```

**字段说明**:
- `valid`: 表项有效标志
- `iova`: 原始IOVA (未对齐，保留页内偏移)
- `next_index`: 指向链表中下一个 entry 的编号，0xFF 表示链表尾
- `tail_index` [V3.0新增]: 
  - **仅链头 entry 有效**: 记录任务链最后一个任务的 Buffer 位置
  - **非链头 entry**: 该值置为 0xFF (BUFFER_CHAIN_END)
- `task_ptr`: 指向原始翻译任务指针

**设计理由**:
- 将 `tail_index` 放在 Buffer 而非 PT Cache，追加任务时只需更新 Buffer 链头 entry 的 `tail_index`，**无需写入 PT Cache**
- 链头 entry 的 `tail_index` 始终指向链表最后一个 entry，方便快速追加
- 非链头 entry 的 `tail_index` 无意义，统一置 0xFF

### 2.3 PTW任务上下文扩展

```cpp
// 文件: iommu/include/iommu_task.hh

struct walk_context_t {
    // 现有字段
    uint64_t iova;
    uint64_t root_ppn;
    int level;
    uint64_t base_addr;
    uint16_t vpn[3];
    uint32_t ptesize;
    
    // [新增] Burst预取相关字段
    bool prefetch_enabled;       // 是否启用预取
    uint32_t prefetch_depth;     // 预取深度 (D=3, 当前仿真配置)
    bool prefetch_burst_pending; // 是否有Burst DDR请求未完成
    uint64_t burst_start_addr;   // Burst读取起始地址
    uint32_t burst_size;         // Burst读取字节数
    std::vector<pte_update_t> pt_updates; // 预取PTE更新列表 (D+1个)
};
```

### 2.4 PTW响应数据结构 (含PTE标记)

```cpp
// 文件: iommu/include/iommu_req_rsp.hh

/// PTE状态标记 (预留,暂不处理错误/无效PTE)
enum class PTEStatus : uint8_t {
    PTE_VALID = 0,          ///< 正常有效PTE (当前统一使用)
    PTE_INVALID = 1,        ///< 无效PTE (V=0)
    PTE_ERROR = 2,          ///< 错误PTE (权限错误等)
    PTE_RESERVED = 3        ///< 保留
};

/// 单个PTE更新 (含状态标记)
struct pte_update_t {
    uint64_t iova;           // 4KB对齐的IOVA
    uint64_t ppn;            // 物理页号
    pte_t pte;               // 完整PTE
    PTEStatus status;        // PTE状态标记 [新增]
    
    // 构造函数
    pte_update_t() : iova(0), ppn(0), status(PTEStatus::PTE_VALID) {}
    pte_update_t(uint64_t i, uint64_t p, pte_t pt, PTEStatus s = PTEStatus::PTE_VALID)
        : iova(i), ppn(p), pte(pt), status(s) {}
};

/// PTW响应 (D+1个结果)
struct ptw_response_t {
    uint32_t task_id;                        // 任务ID
    std::vector<pte_update_t> pt_updates;    // PTE更新列表 (1~D+1个)
    bool is_main_task;                       // 是否主任务 (用于WC更新)
    bool prefetch_enabled;                   // 预取是否启用
    uint32_t prefetch_depth;                 // 实际预取深度
    uint64_t main_iova;                      // 主任务IOVA
    uint64_t actual_page_size;               // 实际页大小 (4KB/2MB/1GB)
    
    // 构造函数
    ptw_response_t() 
        : task_id(0), is_main_task(false), 
          prefetch_enabled(false), prefetch_depth(0),
          main_iova(0), actual_page_size(0x1000) {}
};
```

---

## 三、PT Cache写入流程（V3.0）

### 3.1 完整流程图

```
新请求到达 (iova_raw)
    │
    ├─ Step 1: 对齐iova (去掉低12位) → iova_aligned
    │
    ├─ Step 2: 查询PT Cache (iova_aligned)
    │   │
    │   ├─ [HIT 常规CL] (is_ph=0)
    │   │   └─ 直接返回翻译结果,不发PTW
    │   │
    │   ├─ [HIT 占位CL] (is_ph=1)
    │   │   │
    │   │   ├─ [is_req=1] (主任务已记录)
    │   │   │   ├─ a) 获取占位CL的head_index
    │   │   │   ├─ b) 将任务记录在Buffer位置X
    │   │   │   ├─ c) Buffer[head_index].tail_index的next → X
    │   │   │   ├─     Buffer[head_index].tail_index → X
    │   │   │   └─ 挂起任务,不发PTW
    │   │   │   [V3.0] 无PT Cache写入!
    │   │   │
    │   │   └─ [is_req=0] (预取占位CL,首次任务)
    │   │       ├─ a) 将任务记录在Buffer位置X, X的next=0xFF, tail=0xFF
    │   │       ├─ b) 更新PT Cache: head_index=X, is_req=1
    │   │       ─ 挂起任务,不发PTW
    │   │
    │   └─ [MISS] (无缓存)
    │       │
    │       ├─ 检查Buffer容量
    │       │   ├─ [Buffer已满] → 阻塞等待 (反压)
    │       │   └─ [Buffer有空闲] → 继续
    │       │
    │       ├─ a) 新建占位CL,缓存替换判断
    │       ├─ b) 记录任务到Buffer位置X
    │       ├─ c) 写入PT Cache: head_index=X, is_req=1
    │       ├─ d) 检查预取深度D, D=0则结束
    │       ├─ e) 预设D个占位CL,执行缓存替换
    │       ├─ f) 有空闲CL → 写入PT Cache: head_index=0xFF, is_req=0
    │       ├─ g) 无空闲CL → 丢弃该预取占位CL
    │       ─ 发送PTW请求
    │
    └─ [Cache满兜底]
        ├─ 所有way均为is_req=1占位CL → 无空闲位置
        ├─ 主任务 → 直接转发PTW (不记录Buffer/Cache)
        └─ 预取任务 → 直接丢弃
```

### 3.2 6个分支详细说明

| 分支 | 场景 | PT Cache操作 | Buffer操作 | PT Cache写次数 |
|------|------|-------------|-----------|---------------|
| **1** | HIT常规CL (is_ph=0) | 读 | 无 | **0** |
| **2** | HIT占位CL, is_req=1 | 读 (获取head_index) | 追加: Buffer[head].next=X, Buffer[head].tail=X | **0** [V3.0优化] |
| **3** | HIT占位CL, is_req=0 | 读+写 (head_index=X, is_req=1) | 新建: Buffer[X], next=0xFF, tail=0xFF | **1** |
| **4** | MISS, Buffer满 | 无 | 阻塞等待 | **0** |
| **5** | MISS, Buffer空闲 | 读+写 (主占位CL) | 新建: Buffer[X] | **1** |
| **6** | 插入预取占位CL (D个) | 读+写 (有空闲时) | 无 | **0~D** |

### 3.3 V2.0 vs V3.0 PT Cache写入对比

以50个任务（7页，每页8任务，D=3）为例：

| 操作 | V2.0 PT Cache写次数 | V3.0 PT Cache写次数 | 节省 |
|------|-------------------|-------------------|------|
| Task 1 MISS | 1 (主) + 3 (预取) = 4 | 1 (主) + 3 (预取) = 4 | 0 |
| Task 2~8 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 9 HIT is_req=0 | 1 (更新head+tail+is_req) | 1 (更新head+is_req) | 字段减少 |
| Task 10~16 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 17 HIT is_req=0 | 1 (更新head+tail+is_req) | 1 (更新head+is_req) | 字段减少 |
| Task 18~24 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 25 HIT is_req=0 | 1 (更新head+tail+is_req) | 1 (更新head+is_req) | 字段减少 |
| Task 26~32 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 33 MISS | 1 (主) + 3 (预取) = 4 | 1 (主) + 3 (预取) = 4 | 0 |
| Task 34~40 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 41 HIT is_req=0 | 1 (更新head+is_req) | 1 (更新head+is_req) | 字段减少 |
| Task 42~48 HIT is_req=1 | **7次** (每次更新tail_index) | **0次** (仅写Buffer) | **7次** |
| Task 49 HIT is_req=0 | 1 (更新head+is_req) | 1 (更新head+is_req) | 字段减少 |
| Task 50 HIT is_req=1 | **1次** (每次更新tail_index) | **0次** (仅写Buffer) | **1次** |
| **合计** | **52次** | **14次** | **减少73%** |

---

## 四、替换算法改造

### 4.1 find_victim算法修改

```cpp
// 文件: iommu/cache_src/replacement/plru_policy.cpp

int find_victim(pt_cache_t& cache, uint64_t iova) {
    int victim = -1;
    
    // 第一遍: 寻找非is_req=1的占位CL
    for (int i = 0; i < cache.num_sets; i++) {
        if (cache.sets[i].reserved.is_ph == 1) {
            // 检查is_req标志
            if (cache.sets[i].reserved.is_req == 1) {
                continue;  // 跳过is_req=1的占位CL (绝对不能替换)
            }
            
            // is_req=0的预取占位CL,可以被替换
            if (victim == -1 || is_lru(cache.sets[i], cache.sets[victim])) {
                victim = i;
            }
        }
    }
    
    if (victim != -1) {
        return victim;
    }
    
    // 第二遍: 寻找常规CL
    for (int i = 0; i < cache.num_sets; i++) {
        if (cache.sets[i].reserved.is_ph == 0 && cache.sets[i].valid) {
            if (victim == -1 || is_lru(cache.sets[i], cache.sets[victim])) {
                victim = i;
            }
        }
    }
    
    return victim;  // 可能返回-1 (无可用victim)
}
```

### 4.2 fill函数处理替换失败

```cpp
// 文件: iommu/cache_src/cache/cache_base.cpp

bool fill(pt_cache_t& cache, uint64_t iova, const pte_t& pte) {
    int victim = find_victim(cache, iova);
    
    if (victim == -1) {
        // 无可用victim (所有占位CL的is_req=1,且无常规CL)
        log_warning("PT Cache full, cannot insert iova=0x%lx", iova);
        return false;  // 插入失败
    }
    
    // 执行替换
    cache.sets[victim].valid = 1;
    cache.sets[victim].tag = iova;
    cache.sets[victim].pte = pte;
    cache.sets[victim].reserved.is_ph = 0;  // 转为常规CL
    
    return true;
}
```

### 4.3 替换失败时的兜底逻辑

**场景描述**:
当PT Cache某个set的所有way都是`is_ph=1`且`is_req=1`的占位CL时（即所有Cache line都有实际任务在等待PTW完成），此时新任务查询PT Cache MISS，尝试插入占位CL时会**替换失败**。

**兜底策略**:

1. **主任务占位CL替换失败**:
   - **不**记录到Buffer链表
   - **不**插入PT Cache
   - **直接转发**到PTW模块进行地址翻译
   - 任务不使用去重功能，独立执行PTW

2. **预取占位CL替换失败**:
   - **直接丢弃**该预取占位CL
   - **不**占用Cache资源
   - 继续尝试插入后续的预取占位CL

**代码实现**:

```cpp
// 文件: iommu/cache_src/cache/pt_cache.cpp

bool PTCache::insert_placeholder(...) {
    // 步骤1: 检查Cache set是否所有way都是is_req=1的占位CL
    uint32_t set = hash_function(tag);
    bool all_protected = true;
    bool has_invalid_way = false;
    
    for (uint32_t w = 0; w < num_ways_; w++) {
        if (!cache_array_[set][w].valid) {
            has_invalid_way = true;
            all_protected = false;
            break;
        }
        
        // 检查是否为常规CL或非保护的占位CL
        if (cache_array_[set][w].data.reserved.is_ph == 0) {
            all_protected = false;  // 常规CL，可以被替换
            break;
        }
        
        if (cache_array_[set][w].data.reserved.is_ph == 1 && 
            cache_array_[set][w].data.reserved.is_req == 0) {
            all_protected = false;  // 预取占位CL，可以被替换
            break;
        }
    }
    
    if (all_protected && !has_invalid_way) {
        // 所有way都是受保护的主任务占位CL，无法插入
        std::cout << "[PT_CACHE_FALLBACK] Set " << set << " full, all ways are is_req=1 placeholders." << std::endl;
        if (is_req) {
            std::cout << "Main task placeholder -> Fallback: direct PTW" << std::endl;
        } else {
            std::cout << "Prefetch placeholder -> Fallback: discard" << std::endl;
        }
        return false;  // 替换失败
    }
    
    // 步骤2: 正常插入占位CL
    fill(tag, placeholder_data, false);
    return true;
}
```

**PT Cache响应处理中的兜底分支**:

```cpp
// 文件: iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc

// MISS分支: 去重+预取逻辑
if (PT_CACHE_DEDUP_ENABLED) {
    // 步骤1: 分配Buffer Entry
    uint8_t head_idx = pt_dedup_buffer.allocate_entry();
    
    // 步骤2: 插入主占位CL
    bool insert_success = cache_sub.pt_cache().insert_placeholder(
        task->GSCID, task->PSCID, page_iova, stage, sv48, gstage_x4,
        head_idx, true, &latency);
    
    // [兜底逻辑] 主占位CL插入失败
    if (!insert_success) {
        printf("[DEDUP_FALLBACK] task_id=%u -> Main placeholder insert FAILED, fallback to direct PTW\n",
               task->task_id);
        
        // 释放Buffer entry
        pt_dedup_buffer.free_entry(head_idx);
        pt_dedup_buffer_mtx.unlock();
        
        // 直接发送PTW，不使用去重和预取
        task->walk_ctx.prefetch_enabled = false;
        task->walk_ctx.prefetch_depth = 0;
        task->state = TASK_PTW_REQ;
        
        pt_cache_to_ptw_fifo.write(task);
        continue;
    }
    
    // 步骤3: 插入D个预取占位CL
    for (uint32_t d = 0; d < prefetch_depth; d++) {
        bool prefetch_success = cache_sub.pt_cache().insert_placeholder(
            ..., DEDUP_BUFFER_INVALID_IDX, false, &latency);
        
        if (!prefetch_success) {
            // [兜底逻辑] 预取占位CL插入失败，直接丢弃
            printf("[DEDUP_FALLBACK] Prefetch placeholder[%u] insert FAILED, discarded\n", d);
            // 继续尝试后续的预取占位CL
        }
    }
    
    // 步骤4: 发送PTW请求（带预取）
    // ...
}
```

**设计理由**:

- **主任务兜底直发PTW**: 保证任务不会因Cache满而被阻塞，虽然失去去重优化，但功能正确性不受影响
- **预取占位丢弃**: 预取是性能优化，非必需。丢弃个别预取占位CL不影响主任务的去重和翻译
- **避免死锁**: 如果不提供兜底路径，当Cache full时任务会被阻塞，可能导致系统死锁

---

## 五、PTW Burst预取流程

### 5.1 核心思想

**预取参数D的判断**:
- **D=0**: 预取功能关闭，不执行任何预取相关操作
- **D>0**: 预取功能启用，预取深度为D

**利用最后一级页表连续性**:
- Sv39最后一级页表 (level=0) 存储连续512个PTE (4KB页)
- 相邻4KB页的PTE在内存中连续存放
- 1次Burst读64字节 = 8个PTE (每个PTE=8字节)

**DDR访问优化**:
- 原方案: 8个预取IOVA × 3级walk × 1次DDR = 24次DDR
- 优化方案: 1次Burst读 = 1次DDR
- **降低**: 96%

### 5.2 完整流程

```
主任务PTW完成Level=0 Walk
    │
    ├─ Step 1: 检查预取标志
    │   └─ if (task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0)
    │
    ├─ Step 2: 计算Burst参数
    │   ├─ leaf_pt_base = task->walk_ctx.base_addr
    │   ├─ current_vpn0 = task->walk_ctx.vpn[0]
    │   ├─ burst_start_addr = leaf_pt_base + (current_vpn0 + 1) * ptesize
    │   └─ burst_size = prefetch_depth * ptesize  (8 * 8 = 64字节)
    │
    ├─ Step 3: 发起Burst DDR读请求
    │   ├─ ddr_req.addr = burst_start_addr
    │   ├─ ddr_req.size = burst_size
    │   ├─ ddr_req.is_write = false
    │   └─ ptw_req_ddr_fifo.write(ddr_req)
    │
    ├─ Step 4: 等待Burst DDR响应
    │   └─ task->walk_ctx.walk_phase = PTW_PREFETCH_WAIT
    │
    ├─ Step 5: 接收Burst响应 (64字节)
    │   ├─ 解析PTE[0~7] (8个PTE)
    │   ─ 提取PPN[0~7]
    │
    ├─ Step 6: 构造pt_updates (D+1=9个)
    │   ├─ pt_updates[0]: 主任务结果 (PPN from Level=0 Walk)
    │   ├─ pt_updates[1]: PTE[0] → PPN[0]
    │   ├─ pt_updates[2]: PTE[1] → PPN[1]
    │   └─ ... pt_updates[8]: PTE[7] → PPN[7]
    │
    ─ Step 7: 返回PT Cache更新
        └─ ptw_rsp_fifo.write(pt_updates)
```

### 5.3 关键代码实现

```cpp
// 文件: iommu/iommu_perf_model/iommu_perf_ptw.cc

void iommu_perf_ptw::handle_level0_walk(task_t* task) {
    // ... 现有Level=0 Walk逻辑 ...
    
    // ===== 检查预取参数D =====
    uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
    
    if (prefetch_depth == 0) {
        // 预取功能关闭,直接返回主任务结果
        ptw_response_t response;
        response.task_id = task->task_id;
        response.pt_updates = {task->walk_ctx.main_pte_update};  // 仅1个结果
        response.is_main_task = true;
        response.prefetch_enabled = false;
        
        ptw_rsp_fifo.write(response);
        return;
    }
    
    // 预取功能启用 (D>0)
    if (task->walk_ctx.prefetch_enabled && prefetch_depth > 0) {
        // 计算Burst参数
        uint64_t leaf_pt_base = task->walk_ctx.base_addr;
        uint16_t current_vpn0 = task->walk_ctx.vpn[0];
        uint64_t burst_start_addr = leaf_pt_base + (current_vpn0 + 1) * task->walk_ctx.ptesize;
        uint32_t burst_size = prefetch_depth * task->walk_ctx.ptesize;  // D * 8字节
        
        // 检查页表边界 (不跨越4KB页表页)
        uint64_t pt_page_mask = 0xFFF;  // 4KB页表页
        if ((burst_start_addr & ~pt_page_mask) != (burst_start_addr + burst_size - 1 & ~pt_page_mask)) {
            // Burst跨越页表页边界,截断
            burst_size = (leaf_pt_base | pt_page_mask) - burst_start_addr + 1;
            prefetch_depth = burst_size / task->walk_ctx.ptesize;  // 更新实际预取深度
            task->walk_ctx.prefetch_depth = prefetch_depth;
        }
        
        // 发起Burst DDR读请求
        ddr_req_entry_t burst_req;
        burst_req.task_id = task->task_id;
        burst_req.addr = burst_start_addr;
        burst_req.size = burst_size;
        burst_req.is_write = false;
        
        task->walk_ctx.prefetch_burst_pending = true;
        task->walk_ctx.walk_phase = PTW_PREFETCH_WAIT;
        
        ptw_req_ddr_fifo.write(burst_req);
        return;  // 等待Burst响应
    }
    
    // 无预取,直接返回主任务结果
    // ...
}

void iommu_perf_ptw::handle_burst_response(ddr_rsp_entry_t& rsp) {
    // 接收Burst DDR响应 (64字节)
    uint8_t* burst_data = rsp.data;
    uint32_t prefetch_depth = current_task->walk_ctx.prefetch_depth;
    
    // ===== 构造pt_updates (D+1个) =====
    std::vector<pte_update_t> pt_updates;
    
    // 主任务结果 (已在Level=0 Walk中获取)
    pte_update_t main_update;
    main_update.iova = current_task->walk_ctx.iova & ~0xFFF;
    main_update.ppn = current_task->walk_ctx.main_pte.ppn;
    main_update.pte = current_task->walk_ctx.main_pte;
    main_update.status = PTEStatus::PTE_VALID;
    
    pt_updates.push_back(main_update);
    
    // 预取PTE[0~D-1]
    for (uint32_t i = 0; i < prefetch_depth; i++) {
        uint64_t pte_raw = *(uint64_t*)(burst_data + i * 8);
        pte_t pte = parse_pte(pte_raw);
        
        pte_update_t update;
        update.iova = current_task->walk_ctx.iova + (i + 1) * 0x1000;  // 4KB步长
        update.ppn = pte.ppn;
        update.pte = pte;
        update.status = PTEStatus::PTE_VALID;
        
        pt_updates.push_back(update);
    }
    
    // 返回PT Cache更新 (D+1个结果)
    ptw_response_t response;
    response.task_id = current_task->task_id;
    response.pt_updates = pt_updates;  // D+1个结果
    response.is_main_task = true;
    response.prefetch_enabled = true;
    response.prefetch_depth = prefetch_depth;
    
    ptw_rsp_fifo.write(response);
}
```

### 5.4 跳过Walker Cache查询

```cpp
// Burst预取不查询Walker Cache
// 原因:
// 1. Burst读取的是L0页表的连续PTE,不涉及高层页表
// 2. Walker Cache缓存的是L2/L1页表的PPN,对L0 PTE无用
// 3. 避免无效的WC查询开销

// 实现:
// Burst预取直接在L0页表基地址上偏移读取,不经过WC
uint64_t burst_start_addr = leaf_pt_base + (current_vpn0 + 1) * ptesize;
// 直接发起DDR读,不查询WC
```

---

## 六、PT Cache & Buffer刷新流程（V3.0）

### 6.1 刷新流程总体说明

**核心原则**: Walk操作完成后，**无论是否预取到有效数据，必须返回D+1个结果** (主任务结果 + D个预取结果)。

**预取参数D的判断**:
- **D=0**: 预取功能关闭，不执行任何预取相关操作，仅返回主任务结果 (1个结果)
- **D>0**: 预取功能启用，返回D+1个结果 (主任务 + D个预取)

**Walk返回方式**: Walk返回结果可以分多笔返回 (例如: 主任务先返回，D个预取结果分批次返回)。

**PTE标记机制**: 
- **每笔响应必须携带PTE状态标记** (正常PTE / 错误PTE / 无效PTE)
- **暂不处理错误/无效PTE**，统一按照正常PTE处理
- **预留PTEStatus字段**，未来可扩展错误处理逻辑

### 6.2 刷新流程完整逻辑

```
PTW Walk完成,返回结果Res_A
    │
    ├─ Step 1: 检查预取参数D
    │   ├─ [D=0]: 预取关闭
    │   │   └─ 仅返回主任务结果 (1个pt_update)
    │   │   ─ 跳转到Step 4 (PT Cache刷新)
    │   │
    │   └─ [D>0]: 预取启用
    │       └─ 继续执行预取逻辑 (Step 2~3)
    │
    ├─ Step 2: 执行Burst预取 (仅D>0时)
    │   ├─ Burst读取D个PTE
    │   └─ 解析PTE[0~D-1]
    │
    ├─ Step 3: 构造pt_updates (D+1个)
    │   ├─ pt_updates[0]: 主任务结果Res_A
    │   ├─ pt_updates[1~D]: 预取PTE结果
    │   └─ 返回D+1个结果 (可分多笔返回)
    │
    ├─ Step 4: 检查实际页大小
    │   │
    │   ├─ [大页场景] (实际页大小 > 4KB) **[当前仿真暂不实现]**
    │   │   ├─ Step 4.1: 将大页leaf PTE写入Walker Cache
    │   │   │   └─ WC.insert(leaf_iova, level, ppn)
    │   │   │
    │   │   ├─ Step 4.2: 根据任务A、结果Res_A，构造4KB级iova，在PT Cache中执行刷新
    │   │   │   └─ flush_pt_cache(iova_A, Res_A)
    │   │   │
    │   │   └─ Step 4.3: 根据任务A和预取深度D，构造D个4KB级iova，在PT Cache中执行刷新
    │   │       └─ for i = 1 to D:
    │   │           └─ flush_pt_cache(iova_A + i*4KB, Res_A+i)
    │   │
    │   ─ [4KB标准页场景] (实际页大小 = 4KB) **[当前仿真仅考虑此场景]**
    │       ├─ Step 4.1: 根据任务A和结果Res_A，构造iova并在PT Cache中执行刷新 → 执行Step 5
    │       │   └─ flush_pt_cache(iova_A, Res_A)
    │       │
    │       └─ Step 4.2: 根据任务A、结果Res_A、预取深度D，构造D个4KB级iova，在PT Cache中执行刷新 → D个iova，每个执行Step 5
    │           └─ for i = 1 to D:
    │               └─ flush_pt_cache(iova_A + i*4KB, Res_A+i)
    │
    └─ Step 5: PT Cache刷新操作 (flush_pt_cache)
        │
        ├─ 查询PT Cache (iova_aligned)
        │   │
        │   ├─ [MISS]: 未命中
        │   │   ├─ 判断真实page size
        │   │   │   ├─ [大页]: 结束 (不写入PT Cache)
        │   │   │   ─ [4KB]: 新建常规Cache line并写入PT Cache
        │   │   │       └─ pt_cache.insert_regular(iova, pte)
        │   │
        │   ├─ [HIT 常规CL]: 命中常规Cache line
        │   │   └─ 更新Cache line信息
        │   │       └─ pt_cache.update(iova, pte)
        │   │
        │   └─ [HIT 占位CL]: 命中占位Cache line
        │       ├─ Step 5.1: 从占位CL获取head_index、is_req信息
        │       │
        │       ├─ Step 5.2: 更新占位CL状态
        │       │   ├─ [4KB页]: 占位CL → 常规CL
        │       │   │   └─ CL.is_ph = 0, CL.vs_pte = pte, CL.head_index = 0xFF, CL.is_req = 0
        │       │   └─ [大页]: 直接删除占位CL
        │       │       └─ CL.valid = 0
        │       │
        │       └─ Step 5.3: 处理Buffer链表 (仅is_req=1时)
        │           ├─ if (is_req == 1):
        │           │   ├─ 根据head_index获得任务链指针
        │           │   ├─ cur = head_index
        │           │   │
        │           │   ├─ while (cur != 0xFF):
        │           │   │   ├─ entry = buffer[cur]
        │           │   │   ├─ 根据walk返回结果,完成地址翻译
        │           │   │   │   └─ pa = (ppn << 12) | (entry.iova & 0xFFF)
        │           │   │   ├─ 转发翻译结果
        │           │   │   │   └─ forward_translation(entry.task_ptr, pa)
        │           │   │   ├─ 获取下一个entry
        │           │   │   │   └─ next = entry.next_index
        │           │   │   ├─ 释放当前entry (V=0)
        │           │   │   │   └─ entry.valid = 0
        │           │   │   │   ─ buffer.valid_count--
        │           │   │   ├─ 通知反压阻塞
        │           │   │   │   └─ free_event.notify(SC_ZERO_TIME)
        │           │   │   └─ cur = next
        │           │   │
        │           │   └─ 刷新完任务链全部任务后结束
        │           │
        │           └─ if (is_req == 0):
        │               └─ 预取占位CL,无Buffer链表,仅更新PT Cache
```

### 6.3 关键代码实现

```cpp
// 文件: iommu/cache_src/subsystem/pt_dedup_flush.cc

void flush_pt_cache_after_ptw(ptw_response_t& response,
                               pt_cache_t& pt_cache,
                               dedup_buffer_t& buffer) {
    uint64_t main_iova = response.main_iova;
    uint64_t main_iova_aligned = main_iova & ~0xFFF;  // 4KB对齐
    uint32_t prefetch_depth = response.prefetch_depth;  // 预取深度D
    
    // ===== Step 1: 检查预取参数D =====
    bool prefetch_enabled = (prefetch_depth > 0);
    uint32_t total_updates = prefetch_enabled ? (prefetch_depth + 1) : 1;
    
    // ===== Step 2: 检查实际页大小 =====
    uint64_t actual_page_size = response.actual_page_size;
    bool is_large_page = (actual_page_size > 0x1000);  // > 4KB [当前仿真暂不实现]
    
    // 大页场景: 更新Walker Cache [当前仿真暂不实现]
    if (is_large_page) {
        walker_cache.insert(main_iova, response.level, response.ppn);
    }
    
    // ===== Step 3: 刷新PT Cache (主任务 + D个预取) =====
    for (uint32_t i = 0; i < total_updates; i++) {
        uint64_t iova = prefetch_enabled ? 
            (main_iova_aligned + i * 0x1000) : main_iova_aligned;
        pte_t pte = response.pt_updates[i];
        
        flush_single_pt_cache(iova, pte, actual_page_size, pt_cache, buffer);
    }
}

void flush_single_pt_cache(uint64_t iova, 
                            pte_t pte,
                            uint64_t page_size,
                            pt_cache_t& pt_cache,
                            dedup_buffer_t& buffer) {
    uint64_t iova_aligned = iova & ~0xFFF;
    int cl_index = pt_cache.lookup(iova_aligned);
    
    if (cl_index == -1) {
        // [MISS]: 未命中
        if (page_size > 0x1000) {
            // 大页: 不写入PT Cache,直接结束 [当前仿真暂不实现]
            return;
        } else {
            // 4KB页: 新建常规Cache line
            pt_cache.insert_regular(iova_aligned, pte);
            return;
        }
    }
    
    // [HIT]: 命中
    pt_cache_entry_t& cl = pt_cache.sets[cl_index];
    
    if (cl.reserved.is_ph == 0) {
        // [HIT 常规CL]: 更新Cache line信息
        cl.vs_pte = pte;
        cl.replacement_info = update_lru(cl.replacement_info);
        return;
    }
    
    // [HIT 占位CL]: 复杂处理
    uint8_t head_index = cl.reserved.head_index;
    uint8_t is_req = cl.reserved.is_req;
    
    // Step 5.2: 更新占位CL状态
    if (page_size == 0x1000) {
        // 4KB页: 占位CL → 常规CL
        cl.reserved.is_ph = 0;
        cl.vs_pte = pte;
        cl.reserved.head_index = 0xFF;
        cl.reserved.is_req = 0;
        // [V3.0] 不再需要更新tail_index (已移至Buffer)
    } else {
        // 大页: 删除占位CL [当前仿真暂不实现]
        cl.valid = 0;
        return;  // 大页不处理Buffer链表
    }
    
    // Step 5.3: 处理Buffer链表 (仅is_req=1时)
    if (is_req == 0) {
        return;  // 预取占位CL,无Buffer链表
    }
    
    // is_req=1: 处理任务链
    uint8_t cur = head_index;
    uint64_t ppn = pte.ppn;
    
    while (cur != 0xFF) {
        DedupBufferEntry* entry = &buffer.entries[cur];
        
        // 完成地址翻译
        uint64_t iova_raw = entry->iova;
        uint64_t offset = iova_raw & 0xFFF;
        uint64_t pa = (ppn << 12) | offset;
        
        // 转发翻译结果
        forward_translation(entry->task_ptr, pa);
        
        // 获取下一个entry
        uint8_t next = entry->next_index;
        
        // 释放当前entry
        entry->valid = 0;
        buffer.valid_count--;
        
        // 通知反压阻塞
        free_event.notify(SC_ZERO_TIME);
        
        // 移动到下一个
        cur = next;
    }
}
```

---

## 七、完整端到端流程示例

### 7.1 场景: 连续50个IOVA请求 (预取深度D=3, 512B步长)

**预取参数说明**:
- **D=3**: 预取功能启用，预取深度为3（每个MISS任务插入3个预取占位CL）
- **D=0**: 预取功能关闭，不插入预取占位CL，不执行Burst预取

**重要前提**: PT Cache查询使用**4KB页对齐**的iova (去掉低12位)
- Task1~8: iova=0x10000, 0x10200, ..., 0x10E00 → **对齐后都是0x10000** (Page A)
- Task9~16: iova=0x11000, 0x11200, ..., 0x11E00 → **对齐后都是0x11000** (Page B)
- Task17~24: iova=0x12000, ... → **对齐后都是0x12000** (Page C)
- Task25~32: iova=0x13000, ... → **对齐后都是0x13000** (Page D)
- Task33~40: iova=0x14000, ... → **对齐后都是0x14000** (Page E)
- Task41~48: iova=0x15000, ... → **对齐后都是0x15000** (Page F)
- Task49~50: iova=0x16000, 0x16200 → **对齐后都是0x16000** (Page G)

**页面与任务映射**:

| 页面 | IOVA | 任务编号 | 主任务 | 状态 |
|------|------|---------|--------|------|
| Page A | 0x10000 | Task 1~8 | Task 1 | MISS |
| Page B | 0x11000 | Task 9~16 | Task 9 | HIT is_req=0→1 (Task1预取) |
| Page C | 0x12000 | Task 17~24 | Task 17 | HIT is_req=0→1 (Task1预取) |
| Page D | 0x13000 | Task 25~32 | Task 25 | HIT is_req=0→1 (Task1预取) |
| Page E | 0x14000 | Task 33~40 | Task 33 | **MISS** (未被Task1预取覆盖) |
| Page F | 0x15000 | Task 41~48 | Task 41 | HIT is_req=0→1 (Task33预取) |
| Page G | 0x16000 | Task 49~50 | Task 49 | HIT is_req=0→1 (Task33预取) |

**预取链分析 (关键: HIT is_req=0 不发起预取!)**:
```
Task 1 MISS Page A → 预取 Page B, C, D (插入3个is_req=0的占位CL)
  ├─ Task 9 HIT Page B (is_req=0→1) → 仅更新is_req，【不发起预取】
  ├─ Task 17 HIT Page C (is_req=0→1) → 仅更新is_req，【不发起预取】
  └─ Task 25 HIT Page D (is_req=0→1) → 仅更新is_req，【不发起预取】

Task 33 MISS Page E (未被Task1预取覆盖!) → 预取 Page F, G, H
  ├─ Task 41 HIT Page F (is_req=0→1) → 仅更新is_req，【不发起预取】
  └─ Task 49 HIT Page G (is_req=0→1) → 仅更新is_req，【不发起预取】
```

**关键结论**: 50个任务中只有**2次MISS** (Task1, Task33)

```
时间线 (D=3, Task 1~50):

═══════════════════════════════════════════════════════════════
阶段1: Task 1 MISS + 预取 Page B/C/D
═══════════════════════════════════════════════════════════════

T0: Task1 (iova=0x10000, 512B) 到达 → PT Cache MISS (对齐后iova=0x10000)
    ├─ 分配Buffer[0]: valid=1, iova=0x10000, next=0xFF, tail=0xFF, task_ptr=Task1
    ├─ 插入主占位CL: PT[0x10000].is_ph=1, head_index=0, is_req=1
    ├─ 检查预取参数D=3 (>0,预取启用)
    ├─ 插入预取占位CL (D=3):
    │   ├─ PT[0x11000].is_ph=1, head_index=0xFF, is_req=0  ← Page B
    │   ├─ PT[0x12000].is_ph=1, head_index=0xFF, is_req=0  ← Page C
    │   └─ PT[0x13000].is_ph=1, head_index=0xFF, is_req=0  ← Page D
    └─ 发送PTW请求 (prefetch_enabled=true, depth=3)

═══════════════════════════════════════════════════════════════
阶段2: Task 2~8 追加到 Page A 链表 (零PT Cache写入)
═══════════════════════════════════════════════════════════════

T1: Task2 (iova=0x10200, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x10000)
    ├─ 检测到 is_ph=1, is_req=1 (Task1已记录)
    ├─ 获取 head_index = 0
    ├─ 分配Buffer[1]: valid=1, iova=0x10200, next=0xFF, tail=0xFF, task_ptr=Task2
    ├─ Buffer[head=0].tail_index(=0)的next → 1  (即Buffer[0].next_index = 1)
    ├─ Buffer[head=0].tail_index → 1             (即Buffer[0].tail_index = 1)
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task2,不发PTW

T2: Task3 (iova=0x10400, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x10000)
    ├─ 检测到 is_ph=1, is_req=1
    ├─ 获取 head_index = 0
    ├─ 分配Buffer[2]: valid=1, iova=0x10400, next=0xFF, tail=0xFF, task_ptr=Task3
    ├─ Buffer[head=0].tail_index(=1)的next → 2  (即Buffer[1].next_index = 2)
    ├─ Buffer[head=0].tail_index → 2             (即Buffer[0].tail_index = 2)
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task3,不发PTW

T3: Task4 (iova=0x10600, 512B) 到达 → 同T2模式
    ├─ Buffer[0].next=3, Buffer[0].tail=3
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task4

T4: Task5 (iova=0x10800, 512B) 到达 → 同T2模式
    ├─ Buffer[0].next=4, Buffer[0].tail=4
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task5

T5: Task6 (iova=0x10A00, 512B) 到达 → 同T2模式
    ├─ Buffer[0].next=5, Buffer[0].tail=5
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task6

T6: Task7 (iova=0x10C00, 512B) 到达 → 同T2模式
    ├─ Buffer[0].next=6, Buffer[0].tail=6
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task7

T7: Task8 (iova=0x10E00, 512B) 到达 → 同T2模式
    ├─ Buffer[0].next=7, Buffer[0].tail=7
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task8

此时Buffer链表状态 (Page A):
  Buffer[0]: Task1 (0x10000), next=1, tail=7  ← 链头,tail指向最后一个
  Buffer[1]: Task2 (0x10200), next=2, tail=0xFF
  Buffer[2]: Task3 (0x10400), next=3, tail=0xFF
  Buffer[3]: Task4 (0x10600), next=4, tail=0xFF
  Buffer[4]: Task5 (0x10800), next=5, tail=0xFF
  Buffer[5]: Task6 (0x10A00), next=6, tail=0xFF
  Buffer[6]: Task7 (0x10C00), next=7, tail=0xFF
  Buffer[7]: Task8 (0x10E00), next=0xFF, tail=0xFF
  
  PT[0x10000]: is_ph=1, head_index=0, is_req=1  ← 无tail_index!

═══════════════════════════════════════════════════════════════
阶段3: Task 9 HIT Page B预取占位CL → 升级为is_req=1
═══════════════════════════════════════════════════════════════

T8: Task9 (iova=0x11000, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x11000)
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL,首次有任务)
    ├─ 分配Buffer[8]: valid=1, iova=0x11000, next=0xFF, tail=0xFF, task_ptr=Task9
    ├─ 更新PT[0x11000]: head_index=8, is_req=1  ← [V3.0] 不再写tail_index
    ├─ [V3.0] 仅1次PT Cache写入 (更新head+is_req)
    └─ 挂起Task9,不发PTW

T9: Task10 (iova=0x11200, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x11000)
    ├─ 检测到 is_ph=1, is_req=1
    ├─ 获取 head_index = 8
    ├─ 分配Buffer[9]: valid=1, iova=0x11200, next=0xFF, tail=0xFF, task_ptr=Task10
    ├─ Buffer[head=8].tail_index(=8)的next → 9  (即Buffer[8].next_index = 9)
    ├─ Buffer[head=8].tail_index → 9             (即Buffer[8].tail_index = 9)
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task10

T10: Task11 (iova=0x11400, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=10, Buffer[8].tail=10
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task11

T11: Task12 (iova=0x11600, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=11, Buffer[8].tail=11
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task12

T12: Task13 (iova=0x11800, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=12, Buffer[8].tail=12
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task13

T13: Task14 (iova=0x11A00, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=13, Buffer[8].tail=13
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task14

T14: Task15 (iova=0x11C00, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=14, Buffer[8].tail=14
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task15

T15: Task16 (iova=0x11E00, 512B) 到达 → 同T9模式
    ├─ Buffer[8].next=15, Buffer[8].tail=15
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task16

此时Buffer链表状态 (Page B):
  Buffer[8]: Task9 (0x11000), next=9, tail=15  ← 链头
  Buffer[9]: Task10 (0x11200), next=10, tail=0xFF
  ...
  Buffer[15]: Task16 (0x11E00), next=0xFF, tail=0xFF
  
  PT[0x11000]: is_ph=1, head_index=8, is_req=1

═══════════════════════════════════════════════════════════════
阶段4: Task 17 HIT Page C预取占位CL → 升级为is_req=1
═══════════════════════════════════════════════════════════════

T16: Task17 (iova=0x12000, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x12000)
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL)
    ├─ 分配Buffer[16]: valid=1, iova=0x12000, next=0xFF, tail=0xFF, task_ptr=Task17
    ├─ 更新PT[0x12000]: head_index=16, is_req=1
    ├─ [V3.0] 仅1次PT Cache写入
    └─ 挂起Task17

T17~T23: Task18~24 到达 → 同T9模式 (追加到PT[0x12000]的链表)
    ├─ 每次仅更新Buffer[16]的next和tail
    ├─ [V3.0] 无PT Cache写入!
    └─ 全部挂起

此时Buffer链表状态 (Page C):
  Buffer[16]: Task17 (0x12000), next=17, tail=23  ← 链头
  Buffer[17~23]: Task18~24, tail=0xFF
  
  PT[0x12000]: is_ph=1, head_index=16, is_req=1

═══════════════════════════════════════════════════════════════
阶段5: Task 25 HIT Page D预取占位CL → 升级为is_req=1
═══════════════════════════════════════════════════════════════

T24: Task25 (iova=0x13000, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x13000)
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL)
    ├─ 分配Buffer[24]: valid=1, iova=0x13000, next=0xFF, tail=0xFF, task_ptr=Task25
    ├─ 更新PT[0x13000]: head_index=24, is_req=1
    ├─ [V3.0] 仅1次PT Cache写入
    └─ 挂起Task25

T25~T31: Task26~32 到达 → 同T9模式 (追加到PT[0x13000]的链表)
    ├─ 每次仅更新Buffer[24]的next和tail
    ├─ [V3.0] 无PT Cache写入!
    └─ 全部挂起

此时Buffer链表状态 (Page D):
  Buffer[24]: Task25 (0x13000), next=25, tail=31  ← 链头
  Buffer[25~31]: Task26~32, tail=0xFF
  
  PT[0x13000]: is_ph=1, head_index=24, is_req=1

═══════════════════════════════════════════════════════════════
阶段6: Task 33 MISS Page E (未被Task1预取覆盖!) → 预取 F/G/H
═══════════════════════════════════════════════════════════════

T32: Task33 (iova=0x14000, 512B) 到达 → PT Cache **MISS** (对齐后0x14000)
    ├─ Page E未被Task1预取覆盖 (Task1只预取了B/C/D)
    ├─ 分配Buffer[32]: valid=1, iova=0x14000, next=0xFF, tail=0xFF, task_ptr=Task33
    ├─ 插入主占位CL: PT[0x14000].is_ph=1, head_index=32, is_req=1
    ├─ 插入预取占位CL (D=3):
    │   ├─ PT[0x15000].is_ph=1, head_index=0xFF, is_req=0  ← Page F
    │   ├─ PT[0x16000].is_ph=1, head_index=0xFF, is_req=0  ← Page G
    │   └─ PT[0x17000].is_ph=1, head_index=0xFF, is_req=0  ← Page H
    └─ 发送PTW请求 (prefetch_enabled=true, depth=3)

T33~T39: Task34~40 到达 → 同T1模式 (追加到PT[0x14000]的链表)
    ├─ 每次仅更新Buffer[32]的next和tail
    ├─ [V3.0] 无PT Cache写入!
    └─ 全部挂起

此时Buffer链表状态 (Page E):
  Buffer[32]: Task33 (0x14000), next=33, tail=39  ← 链头
  Buffer[33~39]: Task34~40, tail=0xFF
  
  PT[0x14000]: is_ph=1, head_index=32, is_req=1

═══════════════════════════════════════════════════════════════
阶段7: Task 41 HIT Page F预取占位CL (来自Task33预取) → 升级为is_req=1
═══════════════════════════════════════════════════════════════

T40: Task41 (iova=0x15000, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x15000)
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL,来自Task33预取)
    ├─ 分配Buffer[40]: valid=1, iova=0x15000, next=0xFF, tail=0xFF, task_ptr=Task41
    ├─ 更新PT[0x15000]: head_index=40, is_req=1  ← 【仅更新is_req，不发起预取】
    ├─ [V3.0] 仅1次PT Cache写入
    └─ 挂起Task41

T41~T47: Task42~48 到达 → 同T9模式 (追加到PT[0x15000]的链表)
    ├─ 每次仅更新Buffer[40]的next和tail
    ├─ [V3.0] 无PT Cache写入!
    └─ 全部挂起

此时Buffer链表状态 (Page F):
  Buffer[40]: Task41 (0x15000), next=41, tail=47  ← 链头
  Buffer[41~47]: Task42~48, tail=0xFF
  
  PT[0x15000]: is_ph=1, head_index=40, is_req=1

═══════════════════════════════════════════════════════════════
阶段8: Task 49 HIT Page G预取占位CL (来自Task33预取) → 升级为is_req=1
═══════════════════════════════════════════════════════════════

T48: Task49 (iova=0x16000, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x16000)
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL,来自Task33预取)
    ├─ 分配Buffer[48]: valid=1, iova=0x16000, next=0xFF, tail=0xFF, task_ptr=Task49
    ├─ 更新PT[0x16000]: head_index=48, is_req=1  ← 【仅更新is_req，不发起预取】
    ├─ [V3.0] 仅1次PT Cache写入
    └─ 挂起Task49

T49: Task50 (iova=0x16200, 512B) 到达 → PT Cache HIT 占位CL (对齐后0x16000)
    ├─ 检测到 is_ph=1, is_req=1
    ├─ 获取 head_index = 48
    ├─ 分配Buffer[49]: valid=1, iova=0x16200, next=0xFF, tail=0xFF, task_ptr=Task50
    ├─ Buffer[head=48].tail_index(=48)的next → 49  (即Buffer[48].next_index = 49)
    ├─ Buffer[head=48].tail_index → 49             (即Buffer[48].tail_index = 49)
    ├─ [V3.0] 无PT Cache写入!
    └─ 挂起Task50

此时Buffer链表状态 (Page G):
  Buffer[48]: Task49 (0x16000), next=49, tail=49  ← 链头
  Buffer[49]: Task50 (0x16200), next=0xFF, tail=0xFF
  
  PT[0x16000]: is_ph=1, head_index=48, is_req=1

═══════════════════════════════════════════════════════════════
注入期结束 - 所有50个任务状态汇总
═══════════════════════════════════════════════════════════════

此时Buffer占用: 50个entry (Buffer[0~49])
  PT Cache状态:
    PT[0x10000]: is_ph=1, head_index=0, is_req=1   (8个任务挂起: Task1~8)
    PT[0x11000]: is_ph=1, head_index=8, is_req=1   (8个任务挂起: Task9~16)
    PT[0x12000]: is_ph=1, head_index=16, is_req=1  (8个任务挂起: Task17~24)
    PT[0x13000]: is_ph=1, head_index=24, is_req=1  (8个任务挂起: Task25~32)
    PT[0x14000]: is_ph=1, head_index=32, is_req=1  (8个任务挂起: Task33~40)
    PT[0x15000]: is_ph=1, head_index=40, is_req=1  (8个任务挂起: Task41~48)
    PT[0x16000]: is_ph=1, head_index=48, is_req=1  (2个任务挂起: Task49~50)
    PT[0x17000]: is_ph=1, head_index=0xFF, is_req=0 ← Task33预取 (未被访问)

注意: Task1的预取占位CL (Page B/C/D) 已被Task9/17/25消耗并升级为is_req=1
      Task33的预取占位CL (Page F/G/H) 中，Page F/G已被Task41/49消耗，Page H未被访问
```

═══════════════════════════════════════════════════════════════
Task1 PTW完成 (Burst预取 + Walker Cache更新)
═══════════════════════════════════════════════════════════════

T100: Task1 PTW完成 (假设DDR延时较大,此时50个任务已全部注入)
    │
    ├─ Phase 1: Level=2 Walk (根页表)
    │   ├─ 查询Walker Cache (tag=root_ppn, level=2): MISS
    │   ├─ DDR读取根页表 (1次)
    │   └─ 提取VPN[2]索引,获取L1页表PPN
    │
    ├─ Phase 2: Level=1 Walk (中间页表)
    │   ├─ 查询Walker Cache (tag=L1_ppn, level=1): MISS
    │   ├─ DDR读取L1页表 (1次)
    │   └─ 提取VPN[1]索引,获取L0页表PPN
    │
    ├─ Phase 3: Level=0 Walk (最后一级页表)
    │   ├─ DDR读取L0页表 (1次,获取主任务PTE)
    │   └─ 提取VPN[0]索引,获取最终PPN (pa=0x5000_0000)
    │
    ├─ Phase 4: Burst预取 (读取后续3个PTE, D=3)
    │   ├─ 计算burst_start = L0_base + (VPN[0]+1) * 8
    │   ├─ Burst DDR读24字节 (3个PTE) (1次)
    │   │   └─ 注意: **不查询Walker Cache**,直接从L0页表连续读取
    │   └─ 解析PTE[1~3] → pt_updates[1~3]
    │
    ├─ Phase 5: 更新缓存
    │   ├─ 更新PT Cache (D+1=4个结果):
    │   │   ├─ PT[0x10000]: is_ph=1→0, vs_pte=实际PTE (转为常规CL)
    │   │   ├─ PT[0x11000]: is_ph=1→0, vs_pte=实际PTE (转为常规CL)
    │   │   ├─ PT[0x12000]: is_ph=1→0, vs_pte=实际PTE (转为常规CL)
    │   │   └─ PT[0x13000]: is_ph=1→0, vs_pte=实际PTE (转为常规CL)
    │   │
    │   └─ 更新Walker Cache (仅主任务路径):
    │       ├─ WC.insert(root_ppn, level=2, value=L1_ppn)
    │       └─ WC.insert(L1_ppn, level=1, value=L0_ppn)
    │       └─ 注意: **Burst预取的PTE不更新Walker Cache**
    │
    └─ Phase 6: Flush Buffer链表 (PT[0x10000].head_index=0)
        ├─ cur=0: Task1 (0x10000), pa=(0x5000<<12)|0x000=0x5000_0000, 转发, 释放Buffer[0]
        ├─ cur=1: Task2 (0x10200), pa=(0x5000<<12)|0x200=0x5000_0200, 转发, 释放Buffer[1]
        ├─ cur=2: Task3 (0x10400), pa=(0x5000<<12)|0x400=0x5000_0400, 转发, 释放Buffer[2]
        ├─ cur=3: Task4 (0x10600), pa=(0x5000<<12)|0x600=0x5000_0600, 转发, 释放Buffer[3]
        ├─ cur=4: Task5 (0x10800), pa=(0x5000<<12)|0x800=0x5000_0800, 转发, 释放Buffer[4]
        ├─ cur=5: Task6 (0x10A00), pa=(0x5000<<12)|0xA00=0x5000_0A00, 转发, 释放Buffer[5]
        ├─ cur=6: Task7 (0x10C00), pa=(0x5000<<12)|0xC00=0x5000_0C00, 转发, 释放Buffer[6]
        └─ cur=7: Task8 (0x10E00), pa=(0x5000<<12)|0xE00=0x5000_0E00, 转发, 释放Buffer[7]
        
        Buffer[0~7]全部释放, valid_count从50降至42

═══════════════════════════════════════════════════════════════
后续Task到达 (Task1的预取结果已生效)
═══════════════════════════════════════════════════════════════

T101: Task51 (iova=0x10000, 512B) 到达 → PT Cache HIT 常规CL (is_ph=0)
    └─ 直接返回翻译结果 (pa=0x5000_0000), 不发PTW

T102: Task52 (iova=0x11000, 512B) 到达 → PT Cache HIT 常规CL (is_ph=0)
    └─ 直接返回翻译结果 (pa=0x5001_0000), 不发PTW

... (Task53~54类似,全部HIT常规CL,直接返回)
```

### 7.2 关键观察

1. **PT Cache查询对齐**: 
   - Task1~8 (512B步长) → 对齐后都是0x10000 → **同一个PT Cache条目**
   - Task1创建主占位CL，Task2~8全部HIT并追加到链表

2. **预取占位CL的生命周期**:
   - Task1预取了PT[0x11000, 0x12000, 0x13000] (is_req=0)
   - Task9首次访问PT[0x11000] → 检测到is_req=0 → 置位is_req=1，**不发起预取**
   - Task17首次访问PT[0x12000] → 检测到is_req=0 → 置位is_req=1，**不发起预取**
   - Task25首次访问PT[0x13000] → 检测到is_req=0 → 置位is_req=1，**不发起预取**
   - Task33访问PT[0x14000] → **MISS** (未被Task1预取覆盖) → 预取 F/G/H
   - Task41首次访问PT[0x15000] → 检测到is_req=0 → 置位is_req=1，**不发起预取**
   - Task49首次访问PT[0x16000] → 检测到is_req=0 → 置位is_req=1，**不发起预取** (被Task33预取覆盖)

3. **关键规则: HIT is_req=0 不发起预取**:
   - 只有MISS时才会插入预取占位CL并发起PTW
   - HIT is_req=0 仅更新is_req=0→1，创建Buffer链表头
   - HIT is_req=1 仅追加Buffer链表，零PT Cache写入

4. **Buffer使用效率**:
   - 50个任务占用50个Buffer entry
   - Task1完成后释放8个entry (valid_count: 50→42)
   - 后续每个主任务完成释放对应数量的entry

5. **[V3.0] PT Cache写入大幅减少**:
   - Task2~8追加到链表时，**零PT Cache写入**（仅写Buffer链头entry）
   - Task10~16、Task18~24、Task26~32、Task34~40、Task42~48、Task50同理
   - 相比V2.0，PT Cache写入次数减少约73% (52次→14次)

6. **预取链传递效应 (关键修正)**:
   - Task1的3个预取 → 覆盖3页 (B/C/D)，但**不产生新预取**
   - Task33 MISS → 产生3个预取 (F/G/H)，覆盖Task41/49
   - **HIT is_req=0 不发起预取**，只有MISS才会触发新的预取
   - 50个任务中只有**2次MISS** (Task1, Task33)

### 7.3 Walker Cache交互逻辑

**关键规则**:
1. **主任务** (is_req=1): 需要查询Walker Cache → Walk DDR → 更新Walker Cache
2. **预取任务** (Burst读取的PTE): **不需要**查询Walker Cache，也**不需要**更新Walker Cache

#### 7.3.1 Task1主任务PTW流程 (含Walker Cache)

```
T0: Task1 PTW启动
    │
    ├─ Phase 1: Level=2 Walk (根页表)
    │   ├─ 查询Walker Cache (tag=root_ppn, level=2)
    │   │   ├─ HIT: 直接获取L2 PPN,跳过DDR读
    │   │   └─ MISS: DDR读取根页表 → 更新Walker Cache
    │   └─ 提取VPN[2]索引,获取L1页表PPN
    │
    ├─ Phase 2: Level=1 Walk (中间页表)
    │   ├─ 查询Walker Cache (tag=L1_ppn, level=1)
    │   │   ├─ HIT: 直接获取L1 PPN,跳过DDR读
    │   │   └─ MISS: DDR读取L1页表 → 更新Walker Cache
    │   ─ 提取VPN[1]索引,获取L0页表PPN
    │
    ├─ Phase 3: Level=0 Walk (最后一级页表)
    │   ├─ DDR读取L0页表 (必须访问,获取主任务PTE)
    │   └─ 提取VPN[0]索引,获取最终PPN (pa=0x5000_0000)
    │
    ├─ Phase 4: Burst预取 (读取后续3个PTE, D=3)
    │   ├─ 计算burst_start = L0_base + (VPN[0]+1) * 8
    │   ├─ Burst DDR读24字节 (3个PTE)
    │   │   └─ 注意: **不查询Walker Cache**,直接从L0页表连续读取
    │   └─ 解析PTE[1~3] → pt_updates[1~3]
    │
    └─ Phase 5: 更新缓存
        ├─ 更新PT Cache (4个条目,含主任务+预取)
        ├─ 更新Walker Cache (仅主任务的walk路径):
        │   ├─ 插入/更新: (root_ppn, level=2) → L1_ppn
        │   └─ 插入/更新: (L1_ppn, level=1) → L0_ppn
        │   └─ 注意: **Burst预取的PTE不更新Walker Cache**
        ─ Flush Buffer链表 (Task1~8)
```

#### 7.3.2 Task9流程 (预取占位CL转is_req=1，**不发起PTW**)

```
T8: Task9到达,首次访问PT[0x11000]
    ├─ 检测到 is_ph=1, is_req=0 (预取占位CL,来自Task1预取)
    ├─ 置位is_req=1,创建新链表头
    ├─ 【不发起PTW】 (只有MISS才发起PTW)
    └─ Task9挂起,等待Task1的PTW完成

当Task1 PTW完成时:
    ├─ Phase 5: 更新PT Cache (4个条目: A/B/C/D)
    │   └─ PT[0x11000]: is_ph=1→0, vs_pte=实际PTE (转为常规CL)
    └─ Phase 6: Flush Buffer链表 (PT[0x11000].head_index=8)
        ├─ cur=8: Task9 (0x11000), 转发翻译结果, 释放Buffer[8]
        ├─ cur=9: Task10 (0x11200), 转发翻译结果, 释放Buffer[9]
        ├─ ... (Task11~15类似)
        └─ cur=15: Task16 (0x11E00), 转发翻译结果, 释放Buffer[15]
```

#### 7.3.3 DDR访问次数详细统计 (含Walker Cache优化)

**预取参数D的影响**:
- **D=0**: 不执行预取,仅返回主任务结果 (1个pt_update)
- **D=3**: 执行Burst预取,返回D+1=4个结果

**场景1: D=3 (预取启用)**

| 任务 | L2 Walk | L1 Walk | L0 Walk | Burst预取 | 总DDR | Walker Cache状态 |
|------|---------|---------|---------|----------|-------|------------------|
| Task1 (MISS) | 1次(MISS) | 1次(MISS) | 1次 | 1次 | **4次** | 更新root_ppn, L1_ppn |
| Task2~8 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task9 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=0→1),不发PTW |
| Task10~16 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task17 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=0→1),不发PTW |
| Task18~24 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task25 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=0→1),不发PTW |
| Task26~32 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task33 (MISS) | 0次(HIT) | 0次(HIT) | 1次 | 1次 | **2次** | HIT Task1缓存 |
| Task34~40 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task41 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=0→1),不发PTW |
| Task42~48 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |
| Task49 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=0→1),不发PTW |
| Task50 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT占位CL(is_req=1),不发PTW |

**优化效果分析 (D=3)**:
- **Task1** (MISS Page A): 4次DDR (建立Walker Cache)
- **Task9/17/25** (HIT is_req=0→1): 0次DDR (不发PTW)
- **Task33** (MISS Page E): 2次DDR (Walker Cache HIT)
- **Task41/49** (HIT is_req=0→1): 0次DDR (不发PTW)
- **总DDR**: 4 + 2 = **6次DDR** (只有2次MISS!)

**场景2: D=0 (预取关闭)**

| 任务 | L2 Walk | L1 Walk | L0 Walk | Burst预取 | 总DDR | 备注 |
|------|---------|---------|---------|----------|-------|------|
| Task1 (MISS) | 1次(MISS) | 1次(MISS) | 1次 | 0次 | **3次** | 无预取,仅1个结果 |
| Task2~8 | 0次 | 0次 | 0次 | 0次 | **0次** | HIT主占位CL (不发PTW) |
| Task9 (MISS) | 0次(HIT) | 0次(HIT) | 1次 | 0次 | **1次** | MISS,发起PTW (无预取) |
| Task10~16 | 0次(HIT) | 0次(HIT) | 1次 | 0次 | **1次** | HIT Task9主占位CL (不发PTW) |
| Task17 (MISS) | 0次(HIT) | 0次(HIT) | 1次 | 0次 | **1次** | MISS,发起PTW |
| ... | ... | ... | ... | ... | ... | ... |
| Task49 (MISS) | 0次(HIT) | 0次(HIT) | 1次 | 0次 | **1次** | MISS,发起PTW |
| Task50 | 0次(HIT) | 0次(HIT) | 1次 | 0次 | **1次** | HIT Task49主占位CL |

**对比分析**:
- D=3总DDR: **6次** (2次MISS,预取覆盖效果好)
- D=0总DDR: 3 + 6×1 = **9次** (6次MISS,每页都需要独立PTW)
- **关键差异**: 
  - D=3: Task1预取了3页(B/C/D),Task33预取了3页(F/G/H),只有2次MISS
  - D=0: 每页都需要独立PTW,但有去重(同一4KB页的Task不发PTW),共7页=7次MISS

**注意**: D=0时,虽然有去重 (Task2~8不发PTW),但没有预取占位CL,所以Task9/17/25/33/41/49都需要独立PTW

**对比**: (D=3场景)
- 原方案: 50 × 27 = **1350次DDR**
- 仅Burst优化: 2 × 4 = **8次DDR**
- Burst + Walker Cache: 4 + 1×2 = **6次DDR** (Task33 HIT Task1缓存)
- **总体降低**: 99.6%

#### 7.3.4 Walker Cache更新策略

```
PTW完成后更新Walker Cache的规则:

✅ 主任务Walk路径 (必须更新):
  - 每个level的页表PPN都需要缓存
  - 示例: Task1 walk路径
    * WC.insert(root_ppn, level=2, value=L1_ppn)
    * WC.insert(L1_ppn, level=1, value=L0_ppn)

❌ Burst预取路径 (不更新):
  - Burst读取的PTE[1~8]仅用于更新PT Cache
  - 不更新Walker Cache,因为:
    1. 预取的是L0页表的连续PTE,不涉及高层页表
    2. 避免污染Walker Cache (预取可能不会被使用)
    3. 减少WC写入开销

伪代码实现:
```cpp
// 在PTW响应处理中
void handle_ptw_response(ptw_response_t& resp) {
    // 1. 更新PT Cache (所有结果)
    for (auto& update : resp.pt_updates) {
        pt_cache.write(update.iova, update.pte);
    }
    
    // 2. 更新Walker Cache (仅主任务路径)
    if (resp.is_main_task) {  // 区分主任务和预取任务
        for (auto& wc_entry : resp.walker_cache_updates) {
            walker_cache.insert(wc_entry.tag, wc_entry.level, wc_entry.ppn);
        }
    }
    // 注意: Burst预取的结果不包含在walker_cache_updates中
    
    // 3. Flush Buffer
    flush_dedup_buffer_chain(resp.task_id);
}
```

#### 7.3.5 端到端完整流程 (含Walker Cache)

```
T0: Task1 PTW启动
    ├─ WC查询(root_ppn, L2): MISS → DDR读 (1次)
    ├─ WC查询(L1_ppn, L1): MISS → DDR读 (1次)
    ├─ L0页表: DDR读 (1次)
    ├─ Burst预取: DDR读24字节 (1次, D=3)
    ├─ 更新PT Cache (4个条目)
    ├─ 更新WC: (root_ppn,L2→L1_ppn), (L1_ppn,L1→L0_ppn)
    └─ Flush Buffer[0~7] (Task1~8)

T100: Task9 PTW启动 (来自Task1预取, is_req=0→1, **不发PTW**)
    ├─ Task9挂起,等待Task1的PTW完成
    └─ Task1 PTW完成后, Flush Buffer[8~15] (Task9~16)

T150: Task17/25 (来自Task1预取, is_req=0→1, **不发PTW**)
    ├─ Task17/25挂起,等待Task1的PTW完成
    └─ Task1 PTW完成后, Flush Buffer[16~23] / Buffer[24~31]

T200: Task33 PTW启动 (MISS Page E)
    ├─ WC查询: 全HIT (同root_ppn, L1_ppn)
    ├─ L0页表(0x14000): DDR读 (1次)
    ├─ Burst预取(D=3): DDR读 (1次)
    └─ Flush Buffer[32~39] (Task33~40)

T250: Task41/49 (来自Task33预取, is_req=0→1, **不发PTW**)
    ├─ Task41/49挂起,等待Task33的PTW完成
    └─ Task33 PTW完成后, Flush Buffer[40~47] / Buffer[48~49]
```

#### 7.3.6 关键总结

1. **主任务职责**:
   - 查询Walker Cache (可能HIT,减少DDR)
   - 执行Walk DDR (MISS时需要)
   - 执行Burst预取 (1次连续读)
   - 更新Walker Cache (仅主任务路径)
   - 更新PT Cache (所有结果)
   - Flush Buffer链表

2. **预取任务特点**:
   - Burst读取L0页表连续PTE
   - **不查询Walker Cache** (直接在L0页表读取)
   - **不更新Walker Cache** (仅更新PT Cache)
   - 避免WC污染和写入开销

3. **性能优化效果**:
   - PT Cache去重: Task2~8不发PTW (0次DDR)
   - Burst预取: 1次burst替代4次独立walk (降低75%, D=3)
   - Walker Cache: L2+L1层级HIT,每个主任务省2次DDR (再降50%)
   - **[V3.0] PT Cache写入减少**: 追加任务时零PT Cache写入 (减少约73%)
   - **综合优化**: 99.6% DDR访降低 (50任务仅6次DDR)

---

## 八、边界情况处理

### 8.1 大页降级 **[当前仿真暂不实现]**

```cpp
// 当检测到大页 (2MB/1GB) 时 [当前仿真暂不实现]
if (is_large_page(pte)) {
    // 将大页leaf PTE写入Walker Cache
    walker_cache.insert(leaf_iova, level, ppn);
    
    // 根据大页地址构造4KB级iova,刷新PT Cache
    // 大页覆盖的4KB页范围: [iova_aligned, iova_aligned + page_size)
    for (uint64_t offset = 0; offset < page_size; offset += 0x1000) {
        flush_pt_cache(iova_aligned + offset, pte, page_size, pt_cache, buffer);
    }
}
```

### 8.2 页表边界检查

```cpp
// Burst预取不能跨越页表页边界
uint64_t pt_page_mask = 0xFFF;  // 4KB页表页
if ((burst_start_addr & ~pt_page_mask) != (burst_start_addr + burst_size - 1 & ~pt_page_mask)) {
    // 截断burst_size
    burst_size = (leaf_pt_base | pt_page_mask) - burst_start_addr + 1;
    task->walk_ctx.prefetch_depth = burst_size / task->walk_ctx.ptesize;
}
```

### 8.3 PTE无效处理

```cpp
// 如果Burst读取的PTE无效 (!V=0)
for (uint32_t i = 0; i < prefetch_depth; i++) {
    if (!pte[i].valid) {
        // 方案1: 跳过该PTE,不更新PT Cache
        continue;
        
        // 方案2: 标记为无效占位CL
        pt_cache.insert_placeholder(iova + i*4KB, is_req=0);
    }
}
```

### 8.4 Buffer反压机制

```cpp
// Buffer满时阻塞等待
if (buffer.valid_count == buffer.max_capacity) {
    log_info("Buffer full, blocking new request iova=0x%lx", iova);
    buffer.waiting_for_free = true;
    wait(buffer.free_event);  // SystemC event
    buffer.waiting_for_free = false;
}

// 释放Buffer时通知
void free_buffer_entry(uint8_t index) {
    buffer.entries[index].valid = 0;
    buffer.valid_count--;
    
    if (buffer.waiting_for_free) {
        buffer.free_event.notify();
    }
}
```

---

## 九、修改文件清单

### 9.1 核心文件

| 文件路径 | 修改内容 | 预估行数 |
|---------|---------|---------|
| `iommu/include/iommu_dedup_params.hh` | **[新增]** 配置参数统一定义 (Buffer大小、预取深度、PTE状态等) | +120行 |
| `iommu/cache_src/common/types.h` | pt_reserved_t精简 (移除tail_index, 增加reserved位) | +5行, -5行 |
| `iommu/cache_src/common/dedup_buffer.h` | DedupBufferEntry扩展 (新增tail_index字段) | +5行 |
| `iommu/include/iommu_task.hh` | walk_context_t扩展 (Burst字段) | +15行 |
| `iommu/include/iommu_req_rsp.hh` | **[修改]** 添加PTEStatus, ptw_response_t扩展 | +50行 |
| `iommu/cache_src/cache/pt_cache.cpp` | 写入流程重构 (6分支, 移除tail_index相关写入) | +120行 |
| `iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc` | 响应处理重构 (HIT is_req=1时仅写Buffer) | +80行 |
| `iommu/cache_src/replacement/plru_policy.cpp` | find_victim修改 (保护is_req=1) | +30行 |
| `iommu/iommu_perf_model/iommu_perf_ptw.cc` | Burst预取集成, PTE标记 | +200行 |
| `iommu/cache_src/subsystem/pt_dedup_flush.cc` | flush重构 (批量刷新, PTE状态处理, 大页支持) | +120行 |
| `iommu/iommu_perf_model/iommu_perf_model.hh` | 反压event声明 | +5行 |

**总计**: 约+750行代码修改

### 9.2 不修改的文件

**DC Cache相关文件** (保持现有实现,不添加预取和去重逻辑):
- `iommu/cache_src/cache/dc_cache.cpp` - 不修改
- `iommu/cache_src/cache/dc_cache.hh` - 不修改
- `iommu/cache_src/common/dc_types.h` - 不修改

**原因**: DC Cache (Device Context Cache)不参与本次优化,仅PT Cache实现去重+预取机制。

---

## 十、验证计划

### 10.1 单元测试

- [ ] T1: PT Cache占位CL插入/查询 (验证tail_index不在cacheline中)
- [ ] T2: Buffer链表追加/遍历 (验证链头tail_index正确, 非链头=0xFF)
- [ ] T3: is_req=1占位CL替换保护
- [ ] T4: HIT is_req=1时零PT Cache写入验证
- [ ] T5: Burst预取触发逻辑

### 10.2 集成测试

- [ ] T6: 连续8个IOVA请求 (同4KB页)
- [ ] T7: 连续73个IOVA请求 (跨9个4KB页)
- [ ] T8: Buffer满反压测试
- [ ] T9: Walker Cache HIT/MISS混合
- [ ] T10: Burst预取页表边界截断

### 10.3 性能测试

- [ ] T11: DDR访问次数统计 (目标: 24次/73任务)
- [ ] T12: PT Cache命中率统计
- [ ] T13: PT Cache写入次数统计 (目标: 比V2.0减少58%)
- [ ] T14: Walker Cache命中率统计
- [ ] T15: Buffer平均占用率
- [ ] T16: 端到端延迟统计

### 10.4 边界测试

- [ ] T17: 大页请求 (2MB/1GB)
- [ ] T18: PTE无效场景
- [ ] T19: 页表边界Burst截断
- [ ] T20: Buffer满+高并发
- [ ] T21: 替换算法压力测试

---

## 十一、实施步骤与时间表

### Phase 1: 数据结构改造 (1天)

1. 修改`pt_reserved_t` (移除tail_index, 调整reserved位)
2. 扩展`DedupBufferEntry` (新增tail_index字段)
3. 扩展`walk_context_t`
4. 编译验证

### Phase 2: PT Cache写入流程重构 (2天)

1. 实现6分支写入逻辑
2. Buffer链表管理 (链头tail_index维护)
3. HIT is_req=1时仅写Buffer (零PT Cache写入)
4. 反压机制
5. 单元测试 (T1-T4)

### Phase 3: 替换算法改造 (0.5天)

1. 修改find_victim
2. fill失败处理
3. 单元测试 (T3)

### Phase 4: PTW Burst预取集成 (2.5天)

1. 实现预取触发逻辑
2. Burst DDR请求/响应
3. 跳过Walker Cache查询
4. 集成测试 (T5, T10)

### Phase 5: 刷新流程重构 (1.5天)

1. flush_dedup_buffer_chain重构
2. 批量PT Cache更新
3. 大页场景支持
4. 集成测试 (T6-T7, T17)

### Phase 6: 性能测试与优化 (1天)

1. 运行性能测试 (T11-T16)
2. PT Cache写入次数对比 (V2.0 vs V3.0)
3. 性能分析
4. 优化调整

**总计**: 约8.5天

---

## 十二、风险与缓解措施

### 12.1 风险1: Burst预取跨页表边界

**风险**: Burst读取跨越4KB页表页边界,访问无效地址

**缓解**: 
- 页表边界检查,截断burst_size
- 单元测试T10验证

### 12.2 风险2: Buffer满反压导致死锁

**风险**: 所有Buffer被占位CL占用,新任务无法进入

**缓解**:
- 反压event机制,释放时立即通知
- 监控Buffer使用率,告警阈值80%

### 12.3 风险3: Walker Cache污染

**风险**: Burst预取的PTE更新Walker Cache,导致缓存污染

**缓解**:
- 严格区分主任务/预取任务更新路径
- 代码审查确认`is_main_task`标志正确使用

### 12.4 风险4: 替换算法失败

**风险**: 所有占位CL的is_req=1,无可用victim

**缓解**:
- fill返回false,记录warning日志
- 监控PT Cache命中率,调整容量

### 12.5 风险5: Buffer链头tail_index维护错误

**风险**: 链头entry的tail_index未正确更新,导致追加任务时链表断裂

**缓解**:
- 单元测试T2验证: 追加N个任务后, 链头tail_index=N-1, 非链头tail_index=0xFF
- 代码审查确认: HIT is_req=1时, 始终通过head_index找到链头, 更新链头的tail_index

---

## 十三、总结

本方案整合了三大优化机制:

1. **PT Cache去重+预取**: 占位CL + Buffer链表,实现请求去重和预取占位
2. **PTW Burst预取**: 利用页表连续性,1次burst替代多次walk,降低85% DDR访问
3. **Walker Cache优化**: 主任务查询/更新WC,预取跳过WC,额外降低40% DDR访问

**V3.0核心改进**:
- 将`tail_index`从PT Cache cacheline移至Buffer entry
- HIT is_req=1追加任务时**零PT Cache写入**
- PT Cache写入次数比V2.0减少约**58%**

**最终效果**:
- DDR访问从1971次降至24次 (**降低98.8%**)
- PT Cache写入次数减少约58%
- 理论IOPS提升约82倍
- 适用于连续4KB页访问场景

**下一步**:
1. 评审本方案文档
2. 评审通过后开始代码实施 (Phase 1-6)
3. 完成验证计划 (T1-T21)
4. 性能回归测试

---

**文档结束**  
**版本**: V3.0  
**日期**: 2026-06-09  
**作者**: AI Assistant
