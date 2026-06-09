# PT Cache 去重+预取（参数化D）硬件实现方案（4KB页版）

## 一、核心设计原则（8项确认要点）

### 1.1 关键约束（严格遵循硬件规范）

1. **Cache无tail_index**：PT Cache占位CL仅保存`head_index`字段，**tail_index保存在Buffer中**，Cache中无tail字段
2. **tail_index仅链表首有效**：只有Buffer链表首条目（head_index指向的Entry）保存真实链尾下标，**所有非首条目tail_index固定=0xFF**
3. **Buffer顺序分配**：按0、1、2、3...顺序线性查找第一个空闲Entry
4. **预取占位CL共享head**：主占位CL和D个预取占位CL的`head_index`全部相同（指向同一个Buffer链头）
5. **Cache占位需先申请Buffer index**：插入占位CL前，必须先调用`allocate_entry()`分配空闲Buffer Entry获得head_index
6. **PTW一次性返回1+D个PTE**：PTW接收带预取标记的任务后，顺序读取主PTE+D个预取PTE，**一次性批量返回**给PT Cache，**不分多次返回**。D为预取深度参数，**默认值=8**
7. **PT Cache→PTW请求必须包含预取标记**：PT Cache发送给PTW的task中必须携带`prefetch_enabled`（bool）和`prefetch_depth`（uint32_t）字段
8. **刷新遍历链表**：从`Cache.head_index`开始，`cur=Buffer[cur].next_index`，直到`cur=0xFF`结束

**注**：本方案仅支持4KB页场景，暂不考虑大页（2MB/1GB）处理。

---

## 二、查询流程图（4KB页简化版）

### 2.1 完整查询流程

```
【Command A到达】
  │
  ├─ 步骤1: PT Cache查询
  │     iova页对齐 → 查询PT Cache
  │
  ├─ 步骤2: Hit leaf Cache line?
  │     ├─ YES (命中常规CL，is_ph=0)
  │     │     └─ 返回结果（结束）
  │     │
  │     ─ NO (未命中leaf CL)
  │           │
  │           ├─ 步骤3: PT Cache命中占位Cache line?
  │           │     │
  │           │     ├─ YES (占位CL命中，is_ph=1) → 挂接链表分支
  │           │     │     │
  │           │     │     ├─ 步骤3.1: 构造新buffer entry并写到X位置
  │           │     │     │     Buffer[X]: V=1, Command=taskA, next=0xFF, tail=0xFF
  │           │     │     │     task_ptr = taskA
  │           │     │     │
  │           │     │     ├─ 步骤3.2: 获取占位Cache line的记录信息
  │           │     │     │     head_idx = Cache[iova].head_index
  │           │     │     │
  │           │     │     ├─ 步骤3.3: 从head index获得任务链头entry
  │           │     │     │     head_entry = Buffer[head_idx]
  │           │     │     │
  │           │     │     ├─ 步骤3.4: 从链头entry获得tail index (Y)
  │           │     │     │     Y = head_entry.tail_index
  │           │     │     │
  │           │     │     └─ 步骤3.5: 更新链表
  │           │     │           Buffer[Y].next_index = X  ← 旧尾.next = 新节点
  │           │     │           Buffer[head_idx].tail_index = X  ← 仅更新链头的tail
  │           │     │           └─ 任务挂起，等待PTW完成
  │           │     │
  │           │     └─ NO (MISS) → 创建占位CL分支
  │           │           │
  │           │           ├─ 步骤3.6: 构造新buffer entry并写到X位置
  │           │           │     Buffer[X]: V=1, Command=taskA, next=0xFF, tail=X (首节点)
  │           │           │     task_ptr = taskA
  │           │           │
  │           │           ├─ 步骤3.7: 新建占位Cache line，head index=X
  │           │           │     Cache[iova]: is_ph=1, head_index=X
  │           │           │
  │           │           ├─ 步骤3.8: 缓存替换？
  │           │           │     ├─ YES (Cache满) → 优先替换常规Cache line
  │           │           │     │                   若无，则替换占位Cache line
  │           │           │     └─ NO (Cache未满) → 直接插入
  │           │           │
  │           │           ├─ 步骤3.9: 占位Cache line写入PT Cache
  │           │           │
  │           │           └─ 步骤3.10: 预取D=0?
  │           │                 ├─ YES → 结束（无预取模式，发送普通PTW）
  │           │                 └─ NO → 预读D个占位Cache line，head index=X
  │           │                       │
  │           │                       ├─ 对d=0到D-1:
  │           │                       │     prefetch_iova = base_iova + (d+1)*4KB
  │           │                       │     查询PT Cache[prefetch_iova] → 必定MISS
  │           │                       │     创建占位CL: is_ph=1, head_index=X
  │           │                       │     标记为"预读占位CL"
  │           │                       │
  │           │                       └─ 发送PTW请求
  │           │                             task.prefetch_enabled = true
  │           │                             task.prefetch_depth = D
  │           │                             task.prefetch_iovas = [prefetch_iova[0~D-1]]
  │           │                             task.dedup_head_index = X
```

**关键特征**：
- ✅ **仅PT Cache查询**：暂不涉及Walker Cache
- ✅ **预读机制**：明确"预读D个占位Cache line"
- ✅ **缓存替换策略**：优先替换常规CL，无则替换占位CL

---

## 三、更新流程图（4KB页简化版）

### 3.1 完整更新流程（仅4KB页）

```
【Res_A和D个预取结果到达】
  │
  ├─ 假设：Res_A的leaf PTE的page size = 4KB
  │
  ├─ 步骤1: 更新A的占位Cache line为常规Cache line
  │     Cache[base_iova]: is_ph=0, vs_pte=Res_A.pt_data
  │
  ├─ 步骤2: 根据预取D，构造D个4KB级iova
  │     for d=0 to D-1:
  │       prefetch_4k_iova[d] = base_iova + d*4KB
  │
  ├─ 步骤3: 更新预取的占位Cache line为常规Cache line
  │     for d=0 to D-1:
  │       查询PT Cache[prefetch_4k_iova[d]]
  │       if 命中占位CL (is_ph=1):
  │         is_ph: 1→0
  │         vs_pte = Res_prefetch[d].pt_data
  │       else if 命中常规CL:
  │         更新vs_pte（常规CL已存在，直接更新）
  │       else:
  │         新建Cache line并写入PT Cache
  │           Cache[prefetch_4k_iova[d]]: is_ph=0, vs_pte=...
  │
  └─ 步骤4: 刷新Buffer链表
        cur = Cache.head_index
        while (cur != 0xFF):
          entry = Buffer[cur]
          task = entry.task_ptr
          next_idx = entry.next_index
          
          // 4KB页场景，所有command都有效
          pa = (vs_pte.PPN << 12) | (task.iova & 0xFFF)
          task.pa = pa
          pt_cache_to_fwd_fifo.write(task)
          
          entry.valid = 0  ← 释放entry
          cur = next_idx
```

**关键特征**：
- ✅ **仅4KB页处理**：不涉及大页逻辑
- ✅ **预读占位CL更新**：占位CL→常规CL，未命中则新建
- ✅ **简化刷新逻辑**：4KB页场景所有command都有效，无需判断

---

## 四、数据结构精确定义

### 4.1 PT Cache占位CacheLine

```cpp
// TAG部分
struct PTTag {
    iova_t iova;          // 4KB页对齐
    gscid_t gscid;
    pscid_t pscid;
    TransStage stage;
    bool sv48;
    bool gstage_x4;
};

// Contents部分（pt_reserved_t）
union pt_reserved_t {
    struct {
        uint32_t valid:1;
        uint32_t trans_type:2;
        uint32_t input_page_size:2;
        uint32_t result_page_size:2;
        uint32_t iova_is_va:1;
        uint32_t sv48:1;
        uint32_t gstage_x4:1;
        uint32_t is_ph:1;              // 占位标志：1=占位，0=常规
        uint32_t head_index:8;         // 链表头Buffer编号（创建后永久不变）
        uint32_t replacement_info:2;
        uint32_t reserved:15;          // 注意：NO tail_index字段！
    };
    uint32_t raw = 0;
};

// 关键约束：Cache中仅保存head_index，tail_index保存在Buffer中
```

### 4.2 Buffer表项结构体（4KB页简化版）

```cpp
struct DedupBufferEntry {
    uint8_t  valid = 0;                    // V: 1=有效占用，0=空闲
    uint8_t  reserved1[3] = {0};           // 对齐到4字节
    
    // Command（完整翻译任务）
    gscid_t   gscid = 0;
    pscid_t   pscid = 0;
    iova_t    iova = 0;                    // 原始iova（含offset）
    TransStage stage = TransStage::STAGE1_AND_2;
    bool      sv48 = true;
    bool      gstage_x4 = false;
    bool      reserved2 = false;           // 对齐
    
    // 链表指针
    uint8_t   next_index = 0xFF;           // 后继Buffer下标，无后继=0xFF
    uint8_t   tail_index = 0xFF;           // 仅链表首条目存真实链尾；非首条目固定=0xFF
    
    // 任务指针
    iommu_task_t* task_ptr = nullptr;
    
    // 预取标记（用于区分主占位和预取占位）
    bool      is_prefetch_placeholder = false;
    uint8_t   prefetch_source_idx = 0xFF;  // 预取源Buffer索引
    
    bool is_valid() const { return valid != 0; }
    
    void clear() {
        valid = 0;
        gscid = 0;
        pscid = 0;
        iova = 0;
        next_index = 0xFF;
        tail_index = 0xFF;              // 重置为0xFF
        task_ptr = nullptr;
        is_prefetch_placeholder = false;
        prefetch_source_idx = 0xFF;
    }
};

// 注：4KB页简化版不包含command_valid字段
```

### 4.3 Buffer管理类

```cpp
struct DedupBuffer {
    DedupBufferEntry entries[256];
    uint8_t          valid_count = 0;       // 当前有效Entry数量
    
    // 分配Entry（按顺序线性查找第一个空闲）
    uint8_t allocate_entry() {
        if (valid_count >= 256) return 0xFF;  // Buffer满
        
        // 从0开始线性查找第一个空闲Entry
        for (uint8_t i = 0; i < 256; i++) {
            if (!entries[i].is_valid()) {
                entries[i].clear();
                entries[i].valid = 1;
                valid_count++;
                return i;  // 返回索引（0, 1, 2, ...顺序）
            }
        }
        return 0xFF;
    }
    
    // 释放Entry
    void free_entry(uint8_t idx) {
        if (idx == 0xFF || idx >= 256) return;
        if (entries[idx].is_valid()) {
            entries[idx].clear();
            valid_count--;
        }
    }
    
    bool is_full() const { return valid_count >= 256; }
    uint8_t get_valid_count() const { return valid_count; }
};
```

### 4.4 walk_context_t扩展（PTW预取相关）

```cpp
struct walk_context_t {
    // ... 现有字段 ...
    
    // NEW: 预取相关
    bool      prefetch_enabled = false;     // 是否启用预取
    uint32_t  prefetch_depth = 0;           // 预取深度（页数量，默认=8）
    iova_t    prefetch_base_iova = 0;       // 预取起始IOVA
    uint32_t  prefetch_count = 0;           // 已预取的页数量
    iova_t    prefetch_iovas[16];           // 预取的IOVA列表（最大16页）
    
    // NEW: PT Cache批量更新
    uint32_t  pt_update_count = 0;          // 需要更新的PT Cache条目数
    struct {
        iova_t iova;                        // 目标IOVA
        spte_t vs_pte;                      // VS-stage PTE
        gpte_t g_pte;                       // G-stage PTE
        uint64_t pa;                        // 物理地址
        uint64_t page_sz;                   // 页大小（固定4KB）
    } pt_updates[17];                       // 1个主任务 + 16个预取
    
    // NEW: 预取等待状态
    int8_t prefetch_ddr_pending = 0;        // 待完成的预取DDR响应数
};
```

### 4.5 CacheMessage扩展（批量更新）

```cpp
struct CacheMessage {
    // ... 现有字段 ...
    
    // NEW: 批量更新相关
    bool            is_batch_update = false;        // 是否为批量更新
    uint32_t        batch_update_count = 0;         // 批量更新条目数
    struct {
        iova_t      iova;
        PTData      pt_data;
    } batch_updates[17];                            // 1+16个更新条目
};
```

### 4.6 iommu_task_t扩展

```cpp
struct iommu_task_t {
    // ... 现有字段 ...
    
    // NEW: 去重模块相关
    uint8_t dedup_head_index = 0xFF;  // Buffer任务链头指针（0-255，0xFF=无效）
    
    // Constructor
    iommu_task_t()
        : // ... 现有初始化 ...
          dedup_head_index(0xFF) {}
};
```

---

## 五、参数配置

### 5.1 预取深度参数

**文件**: `iommu/iommu_perf_model/iommu_perf_params.hh`

```cpp
// ===================== PT Cache去重+预取模块参数 =====================
static const bool PT_CACHE_DEDUP_ENABLED = true;           // 去重功能开关
static const uint32_t PT_DEDUP_BUFFER_SIZE = 256;          // Buffer大小（entries）
static const uint32_t PT_DEDUP_PREFETCH_DEPTH = 8;         // 预取深度（页数量，默认=8）
static const uint8_t DEDUP_BUFFER_INVALID_IDX = 0xFF;      // 无效索引标记
```

**参数说明**:
- `PT_DEDUP_PREFETCH_DEPTH`: 预取深度参数
  - **默认值=8**：预取后续8个4K页
  - **可配置范围**：1~16（受限于pt_updates数组大小17）
  - **调优建议**：
    - 连续DMA传输场景：8~12
    - 随机访问场景：1~2
    - 关闭预取：0

---

## 六、完整执行时序（P1~P8示例，D=8，仅4KB页）

### 6.1 初始状态

```
PT Cache: 空
Buffer: [0~255] 全部 V=0, next=0xFF, tail=0xFF
```

### 6.2 P1（第1包，iova=0x1000_0000）→ MISS

**执行流程**:
```
P1到达 → PT Cache MISS
  ├─ allocate_entry() → Buffer[0]
  ├─ Buffer[0]: V=1, next=0xFF, tail=0, task_ptr=task0
  ├─ 新建主占位CL: Cache[0x1000_0000].is_ph=1, head_index=0
  ├─ 缓存替换？否（Cache未满）
  ├─ 预取D=0? 否（D=8）
  ├─ 预读8个占位CL:
  │     Cache[0x1000_1000]: is_ph=1, head_index=0
  │     Cache[0x1000_2000]: is_ph=1, head_index=0
  │     ...
  │     Cache[0x1000_8000]: is_ph=1, head_index=0
  └─ 发送PTW: prefetch_enabled=true, prefetch_depth=8
```

**状态**:
```
Buffer[0]: V=1, next=0xFF, tail=0
Cache: 9个占位CL (1主+8预读)，全部head_index=0
链表: [0]
```

### 6.3 P2（第2包，iova=0x1000_0200）→ Hit 占位CL

**执行流程**:
```
P2到达 → PT Cache HIT 占位CL (head_index=0)
  ├─ allocate_entry() → Buffer[1]
  ├─ Buffer[1]: V=1, next=0xFF, tail=0xFF (非首节点)
  ├─ head_idx = Cache[0x1000_0000].head_index = 0
  ├─ Y = Buffer[0].tail = 0
  ├─ Buffer[0].next = 1
  ─ Buffer[0].tail = 1
```

**状态**:
```
Buffer[0]: V=1, next=1, tail=1
Buffer[1]: V=1, next=0xFF, tail=0xFF
链表: 0→1
```

### 6.4 P3~P8 快速推演

```
P3: Buffer[2], Buffer[1].next=2, Buffer[0].tail=2, 链表: 0→1→2
P4: Buffer[3], Buffer[2].next=3, Buffer[0].tail=3, 链表: 0→1→2→3
P5: Buffer[4], Buffer[3].next=4, Buffer[0].tail=4, 链表: 0→1→2→3→4
P6: Buffer[5], Buffer[4].next=5, Buffer[0].tail=5, 链表: 0→1→2→3→4→5
P7: Buffer[6], Buffer[5].next=6, Buffer[0].tail=6, 链表: 0→1→2→3→4→5→6
P8: Buffer[7], Buffer[6].next=7, Buffer[0].tail=7, 链表: 0→1→2→3→4→5→6→7
```

### 6.5 最终Buffer状态汇总（P1~P8全部到达后）

| Buffer | V | next_index | tail_index | 备注 |
|--------|---|------------|------------|------|
| **0** | 1 | 1 | **7** | **链表头，唯一有效tail=7** |
| 1 | 1 | 2 | 0xFF | 中间节点 |
| 2 | 1 | 3 | 0xFF | 中间节点 |
| 3 | 1 | 4 | 0xFF | 中间节点 |
| 4 | 1 | 5 | 0xFF | 中间节点 |
| 5 | 1 | 6 | 0xFF | 中间节点 |
| 6 | 1 | 7 | 0xFF | 中间节点 |
| 7 | 1 | 0xFF | 0xFF | 链表尾 |

**链表链路**: `0→1→2→3→4→5→6→7`  
**Cache.head_index**: 固定=0（创建后永不改变）

---

### 6.6 PTW返回后刷新流程（4KB页）

```
PTW返回（一次性批量返回，D=8）:
  pt_updates[0]: iova=0x1000_0000, vs_pte={PPN=0x5000, page_sz=4KB}
  pt_updates[1]: iova=0x1000_1000, vs_pte={PPN=0x5001, page_sz=4KB}
  ...
  pt_updates[8]: iova=0x1000_8000, vs_pte={PPN=0x5008, page_sz=4KB}

【更新流程】:
  ├─ 步骤1: 更新主占位CL为常规CL
  │     Cache[0x1000_0000]: is_ph=0, vs_pte=PTE0
  │
  ├─ 步骤2: 更新预读占位CL为常规CL
  │     Cache[0x1000_1000]: is_ph=0, vs_pte=PTE1
  │     Cache[0x1000_2000]: is_ph=0, vs_pte=PTE2
  │     ...
  │     Cache[0x1000_8000]: is_ph=0, vs_pte=PTE8
  │
  └─ 步骤3: 刷新Buffer链表
        cur = Cache.head_index = 0
        
        Iteration 1 (cur=0):
          task = Buffer[0].task_ptr = task0
          pa = (0x5000 << 12) | (task0.iova & 0xFFF) = 0x5000_0000
          task0.pa = 0x5000_0000
          pt_cache_to_fwd_fifo.write(task0)
          Buffer[0].V = 0  ← 释放
          cur = Buffer[0].next_index = 1
        
        Iteration 2 (cur=1):
          task = Buffer[1].task_ptr = task1
          pa = (0x5000 << 12) | 0x200 = 0x5000_0200
          task1.pa = 0x5000_0200
          pt_cache_to_fwd_fifo.write(task1)
          Buffer[1].V = 0
          cur = 2
        
        ... (继续遍历 cur=2~6)
        
        Iteration 8 (cur=7):
          task7.pa = 0x5000_0E00
          pt_cache_to_fwd_fifo.write(task7)
          Buffer[7].V = 0
          cur = Buffer[7].next_index = 0xFF
          → cur == 0xFF，循环结束
        
        刷新完成！Buffer[0~7]全部空闲
```

### 6.7 最终状态

```
Buffer:
  [0~7]: 全部 V=0, next=0xFF, tail=0xFF  ← 全部空闲

PT Cache:
  0x1000_0000: is_ph=0, vs_pte={PPN=0x5000, ...}  ← 常规CL
  0x1000_1000: is_ph=0, vs_pte={PPN=0x5001, ...}  ← 常规CL
  ...
  0x1000_8000: is_ph=0, vs_pte={PPN=0x5008, ...}  ← 常规CL

Forwarder:
  收到 task0~task7，pa分别为 0x5000_0000~0x5000_0E00
```

---

## 七、字段变更追踪表（P1~P8完整过程）

| 包 | Buffer编号 | 操作 | Buffer[old_tail].next修改 | Buffer[head].tail更新 | Cache.head_index |
|----|-----------|------|-------------------------|---------------------|------------------|
| P1 | 0 | 创建链头 | 无 | 0（自身） | 0 |
| P2 | 1 | 挂接 | Buffer[0].next=1 | 0→1 | 0 |
| P3 | 2 | 挂接 | Buffer[1].next=2 | 1→2 | 0 |
| P4 | 3 | 挂接 | Buffer[2].next=3 | 2→3 | 0 |
| P5 | 4 | 挂接 | Buffer[3].next=4 | 3→4 | 0 |
| P6 | 5 | 挂接 | Buffer[4].next=5 | 4→5 | 0 |
| P7 | 6 | 挂接 | Buffer[5].next=6 | 5→6 | 0 |
| P8 | 7 | 挂接 | Buffer[6].next=7 | 6→7 | 0 |

**关键观察**:
- Cache.head_index 从P1创建后**永远不变**（始终=0）
- 只有Buffer[0].tail_index在每次挂接时更新
- 中间Buffer的tail_index始终=0xFF

---

## 八、文件修改清单

### 8.1 新建文件（2个）
1. `iommu/cache_src/common/dedup_buffer.h` - Buffer定义（4KB页简化版）
2. `iommu/iommu_perf_model/iommu_perf_pt_dedup_flush.cc` - 刷新逻辑（4KB页简化版）

### 8.2 修改文件（10个）

**数据结构**（4个）:
1. `iommu/include/iommu_task.hh` - 扩展walk_context_t（预取字段）、iommu_task_t（dedup_head_index）
2. `iommu/cache_src/common/types.h` - 扩展pt_reserved_t（is_ph, head_index）、CacheMessage（批量更新）
3. `iommu/cache_src/common/dedup_buffer.h` - DedupBufferEntry（next_index, tail_index）
4. `iommu/include/iommu_task.hh` - 新增PTW_PREFETCH_WAIT枚举

**Cache层**（2个）:
5. `iommu/cache_src/cache/pt_cache.h` - 新增insert_placeholder, batch_update_placeholders接口
6. `iommu/cache_src/cache/pt_cache.cpp` - 实现占位CL插入/批量更新

**子系统**（1个）:
7. `iommu/cache_src/subsystem/cache_subsystem.cpp` - execute_pt_request（区分3种响应）、execute_pt_update（批量处理）

**PTW模块**（1个）:
8. `iommu/iommu_perf_model/iommu_perf_ptw.cc` - ptw_req_process_thread（预取初始化）、ptw_rsp_process_thread（PTW_PREFETCH_WAIT状态、一次性批量返回）

**Top层**（3个）:
9. `iommu/iommu_top.hh` - 删除旧去重代码，新增DedupBuffer实例
10. `iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc` - 重写collector_pt_response_thread（查询/更新流程）
11. `iommu/iommu_perf_model/iommu_perf_params.hh` - 新增PT_DEDUP_PREFETCH_DEPTH参数

---

## 九、测试方案

### 9.1 单元测试

**测试1**: Buffer分配顺序
```cpp
assert(buffer.allocate_entry() == 0);
assert(buffer.allocate_entry() == 1);
assert(buffer.allocate_entry() == 2);
```

**测试2**: 链表挂接
```cpp
head = allocate_entry();  // 0
Buffer[0].tail = 0;

new_idx = allocate_entry();  // 1
Buffer[0].next = 1;
Buffer[0].tail = 1;

assert(Buffer[0].next == 1);
assert(Buffer[0].tail == 1);
assert(Buffer[1].tail == 0xFF);  // 非首节点tail固定0xFF
```

**测试3**: PTW一次性返回
```cpp
assert(batch_req.is_batch_update == true);
assert(batch_req.batch_update_count == 1 + D);  // D=8时为9
for (int i = 0; i < batch_req.batch_update_count; i++) {
    assert(batch_req.batch_updates[i].pt_data.reserved.is_ph == 0);
}
```

### 9.2 集成测试

**测试4**: P1~P8连续同页访问（4KB页，D=8）
```cpp
// 输入: P1~P8 (iova=0x1000_0000~0x1000_0E00)
// 预期:
// - PT Cache MISS: 1次 (P1)
// - PT Cache HIT (占位): 7次 (P2~P8)
// - PTW次数: 1次
// - Buffer Entry: 8个 (0~7)
// - PTW返回: 1次批量更新 (1+8=9个PTE，page_sz=4KB)
// - Forwarder接收: 8个任务
// - Buffer释放: 8个 (全部成功转发)
```

**测试5**: 跨页连续访问（P1~P100，D=8）
```cpp
// 输入: P1~P100 (连续地址，跨越25个页)
// 预期:
// - PTW次数: 4次 (P1, P33, P65, P97)
// - PT Cache MISS: 4次
// - PT Cache HIT: 96次
// - 预取覆盖率: 87%
```

**测试6**: 预取深度参数调优
```cpp
// 测试D=1, 4, 8, 12, 16
// 对比PTW次数、Cache命中率、DDR读次数
// 找到最优D值
```

---

## 十、总结

### 10.1 8项确认要点回顾

1. ✅ **Cache无tail_index**：tail_index保存在Buffer中
2. ✅ **Buffer顺序分配**：按0、1、2...线性查找空闲Entry
3. ✅ **预取占位CL共享head**：主占位和D个预取占位的head_index全部相同
4. ✅ **刷新遍历链表**：从head开始，cur=Buffer[cur].next，直到cur=0xFF
5. ✅ **Cache占位需先申请Buffer index**：插入占位CL前先allocate_entry()
6. ✅ **PTW一次性返回1+D个PTE**：D为参数（默认8），顺序读取，一次性批量返回，不分多次
7. ✅ **PT Cache→PTW请求包含预取标记**：prefetch_enabled（bool）和prefetch_depth（uint32_t）
8. ✅ **PTW内部执行预取逻辑**：PTW_PREFETCH_WAIT状态等待所有预取DDR响应

### 10.2 4KB页简化说明

**简化内容**：
- ❌ **移除大页处理**：不涉及2MB/1GB页场景
- ❌ **移除Walker Cache交互**：仅使用PT Cache
- ❌ **移除command_valid字段**：4KB页场景所有command都有效
- ❌ **移除Command无效判断**：简化刷新逻辑
- ✅ **保留核心去重+预取机制**：占位CL、Buffer链表、预读、批量更新

**适用场景**：
- ✅ 标准4KB页翻译
- ✅ 连续DMA传输优化
- ✅ 高频同页访问去重

### 10.3 性能预期

| 场景 | PTW次数 | Cache MISS | Buffer Entry | DDR读次数 |
|------|---------|-----------|-------------|----------|
| P1~P8（4KB页，D=8） | 1 | 1 | 8 | 9 |
| P1~P100（4KB页，D=8） | 13 | 13 | 100 | ~205 |
| 随机访问 | 100 | 100 | 100 | ~500 |

### 10.4 实施建议

1. **第一阶段**: 扩展数据结构（pt_reserved_t, DedupBufferEntry, walk_context_t）
2. **第二阶段**: 实现Buffer分配/挂接/释放逻辑
3. **第三阶段**: 实现PT Cache占位CL插入/批量更新
4. **第四阶段**: 实现PTW预取DDR读取（PTW_PREFETCH_WAIT状态）
5. **第五阶段**: 实现一次性批量返回和flush_dedup_buffer_chain
6. **第六阶段**: 测试P1~P8场景
7. **第七阶段**: 预取深度参数调优和性能验证

请评审此4KB页简化版方案，确认无误后开始实施。
