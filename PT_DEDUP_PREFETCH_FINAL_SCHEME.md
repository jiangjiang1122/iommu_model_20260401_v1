# PT Cache 去重+预取 D=8 硬件实现方案（完整确认版）

## 一、核心设计原则（8项确认要点）

### 1.1 关键约束（严格遵循硬件规范）

1. **Cache无tail_index**：PT Cache占位CL仅保存`head_index`字段，**tail_index保存在Buffer中**，Cache中无tail字段
2. **tail_index仅链表首有效**：只有Buffer链表首条目（head_index指向的Entry）保存真实链尾下标，**所有非首条目tail_index固定=0xFF**
3. **Buffer顺序分配**：按0、1、2、3...顺序线性查找第一个空闲Entry
4. **预取占位CL共享head**：主占位CL和8个预取占位CL的`head_index`全部相同（指向同一个Buffer链头）
5. **Cache占位需先申请Buffer index**：插入占位CL前，必须先调用`allocate_entry()`分配空闲Buffer Entry获得head_index
6. **PTW一次性返回1+8个PTE**：PTW接收带预取标记的任务后，顺序读取主PTE+8个预取PTE，**一次性批量返回**给PT Cache，**不分多次返回**
7. **PT Cache→PTW请求必须包含预取标记**：PT Cache发送给PTW的task中必须携带`prefetch_enabled=true`和`prefetch_depth=8`字段
8. **刷新遍历链表**：从`Cache.head_index`开始，`cur=Buffer[cur].next_index`，直到`cur=0xFF`结束

---

### 1.2 PTW预取执行流程（核心设计）

```
【PT Cache MISS分支】
Task0到达 (iova=0x1000_0000)
  ├─ 步骤1: 查询PT Cache → MISS
  ├─ 步骤2: allocate_entry() → 获得Buffer[0]
  ├─ 步骤3: 填充Buffer[0]
  │     V=1, next=0xFF, tail=0 (首节点tail=自身)
  │     task_ptr = task0
  ├─ 步骤4: 插入主占位CL
  │     Cache[0x1000_0000]: is_ph=1, head_index=0
  ├─ 步骤5: 插入8个预取占位CL
  │     Cache[0x1000_1000]: is_ph=1, head_index=0
  │     Cache[0x1000_2000]: is_ph=1, head_index=0
  │     ...
  │     Cache[0x1000_8000]: is_ph=1, head_index=0
  └─ 步骤6: 发送PTW请求
        task.prefetch_enabled = true      ← 预取标记
        task.prefetch_depth = 8           ← 预取深度
        task.prefetch_iovas = [0x1000_1000, 0x1000_2000, ..., 0x1000_8000]
        task.dedup_head_index = 0         ← Buffer链头索引


【PTW执行流程】
PTW接收task0:
  ├─ 步骤1: Walk主地址 0x1000_0000
  │     读取Page Table → 获取PTE0 (PPN=0x5000)
  │     task.walk_ctx.pt_updates[0].vs_pte = PTE0
  │
  ├─ 步骤2: 检测到 prefetch_enabled=true
  │     启动预取DDR读取序列
  │
  ├─ 步骤3: 顺序读取预取PTE1 (iova=0x1000_1000)
  │     计算PTE地址 → 发送DDR读请求 → 等待响应
  │     task.walk_ctx.pt_updates[1].vs_pte = PTE1 (PPN=0x5001)
  │
  ├─ 步骤4: 顺序读取预取PTE2 (iova=0x1000_2000)
  │     task.walk_ctx.pt_updates[2].vs_pte = PTE2 (PPN=0x5002)
  │
  ├─ 步骤5~9: 继续读取PTE3~PTE8
  │     ...
  │
  ├─ 步骤10: 检查所有预取完成
  │     pt_updates[0~8].vs_pte 全部有效？
  │     → YES: walk_complete = true
  │
  └─ 步骤11: 一次性批量返回PT Cache
        CacheMessage.is_batch_update = true
        CacheMessage.batch_update_count = 9
        CacheMessage.batch_updates[0].iova = 0x1000_0000, pt_data = {PTE0, ...}
        CacheMessage.batch_updates[1].iova = 0x1000_1000, pt_data = {PTE1, ...}
        ...
        CacheMessage.batch_updates[8].iova = 0x1000_8000, pt_data = {PTE8, ...}
        
        cache_sub.pt_update_fifo.write(CacheMessage)  ← 一次性写入


【PT Cache批量更新】
PT Cache接收批量更新:
  ├─ 步骤1: 遍历batch_updates[0~8]
  ├─ 步骤2: 对每个iova查找占位CL
  ├─ 步骤3: is_ph: 1→0, head_index: 0→0
  └─ 步骤4: 更新vs_pte为实际翻译结果


【Buffer链表刷新】
flush_dedup_buffer_chain(task0):
  cur = Cache.head_index = 0
  
  Iteration 1 (cur=0):
    task = Buffer[0].task_ptr = task0
    pa = (PTE0.PPN << 12) | (task0.iova & 0xFFF)
       = (0x5000 << 12) | 0x000 = 0x5000_0000
    task0.pa = 0x5000_0000
    转发: pt_cache_to_fwd_fifo.write(task0)
    Buffer[0].V = 0  ← 释放
    cur = Buffer[0].next_index = 1
  
  Iteration 2 (cur=1):
    task = Buffer[1].task_ptr = task1
    pa = (0x5000 << 12) | 0x200 = 0x5000_0200
    task1.pa = 0x5000_0200
    转发task1
    Buffer[1].V = 0
    cur = Buffer[1].next_index = 2
  
  ... (继续遍历 cur=2~7)
  
  Iteration 8 (cur=7):
    task7.pa = 0x5000_0E00
    转发task7
    Buffer[7].V = 0
    cur = Buffer[7].next_index = 0xFF
    → cur == 0xFF，循环结束
  
  刷新完成！Buffer[0~7]全部空闲
```

---

## 二、数据结构精确定义

### 2.1 PT Cache占位CacheLine

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

### 2.2 Buffer表项结构体

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
```

### 2.3 Buffer管理类

```cpp
struct DedupBuffer {
    DedupBufferEntry entries[256];
    uint8_t          valid_count = 0;       // 当前有效Entry数量
    uint8_t          free_list_head = 0;    // 空闲链表头（优化查找）
    
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

### 2.4 walk_context_t扩展（PTW预取相关）

```cpp
struct walk_context_t {
    // ... 现有字段 ...
    
    // NEW: 预取相关
    bool      prefetch_enabled = false;     // 是否启用预取
    uint32_t  prefetch_depth = 0;           // 预取深度（页数量）
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
        uint64_t page_sz;                   // 页大小
    } pt_updates[17];                       // 1个主任务 + 16个预取
    
    // NEW: 预取等待状态
    int8_t prefetch_ddr_pending = 0;        // 待完成的预取DDR响应数
};
```

### 2.5 CacheMessage扩展（批量更新）

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

### 2.6 iommu_task_t扩展

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

## 三、通用硬件执行流程

### 3.1 写入流程（完整）

```
任务A到达 → 查询PT Cache
│
├─ ① 命中常规CL (Is_ph=0)
│   └─ 返回SPA结果，结束
│
├─ ② 命中占位CL (Is_ph=1)
│   ├─ allocate_entry() → 获得Buffer[N]
│   ├─ Buffer[N]: V=1, Command=任务A, next=0xFF, tail=0xFF
│   ├─ 从Cache读出 head_idx = Cache.head_index
│   ├─ OldTail = Buffer[head_idx].tail_index
│   ├─ Buffer[OldTail].next_index = N
│   └─ Buffer[head_idx].tail_index = N  ← 仅更新链表首的tail
│       └─ 结束，不发PTW
│
└─ ③ 未命中
    ├─ allocate_entry() → 获得Buffer[X]
    ├─ Buffer[X]: V=1, Command=任务A, next=0xFF, tail=X (首节点)
    ├─ 插入主占位CL: Cache[iova].is_ph=1, head_index=X
    ├─ 插入D个预取占位CL: Cache[iova+d*4K].is_ph=1, head_index=X (d=1~D)
    └─ 发送PTW请求:
        task.prefetch_enabled = true
        task.prefetch_depth = D
        task.prefetch_iovas = [iova+4K, iova+8K, ..., iova+D*4K]
        task.dedup_head_index = X
```

### 3.2 PTW执行流程（一次性返回1+D个PTE）

```
PTW接收task:
│
├─ 步骤1: Walk主地址 (task.iova)
│   └─ 读取Page Table → 获取PTE0
│       task.walk_ctx.pt_updates[0].vs_pte = PTE0
│
├─ 步骤2: 检查 prefetch_enabled
│   ├─ false → 直接walk_complete=true
│   └─ true → 进入预取流程
│
─ 步骤3: 顺序读取预取PTE1~PTED
│   ├─ for d = 1 to prefetch_depth:
│   │     ├─ 计算PTE地址: pte_addr = base_addr + vpn_d * PTESIZE
│   │     ├─ 发送DDR读请求
│   │     ├─ 等待DDR响应
│   │     ├─ 解析PTE: task.walk_ctx.pt_updates[d].vs_pte = PTE_d
│   │     └─ prefetch_ddr_pending--
│   │
│   └─ 等待所有预取完成: prefetch_ddr_pending == 0
│
├─ 步骤4: 构造批量更新消息
│   ├─ CacheMessage.is_batch_update = true
│   ├─ CacheMessage.batch_update_count = 1 + prefetch_depth
│   ├─ for i = 0 to prefetch_depth:
│   │     ├─ batch_updates[i].iova = pt_updates[i].iova
│   │     └─ batch_updates[i].pt_data = 构造PTData(pt_updates[i])
│   ─ cache_sub.pt_update_fifo.write(CacheMessage)  ← 一次性写入
│
─ 步骤5: 调用flush_dedup_buffer_chain(task)
```

### 3.3 PT Cache批量更新流程

```
PT Cache接收批量更新:
│
─ 检查 is_batch_update
│   ├─ false → 单个更新（原有逻辑）
│   └─ true → 批量更新
│
├─ 批量更新流程:
│   ├─ for i = 0 to batch_update_count-1:
│   │     ├─ iova = batch_updates[i].iova
│   │     ├─ 查询PT Cache[iova]
│   │     ├─ if 命中占位CL (is_ph=1):
│   │     │     ├─ 更新data = batch_updates[i].pt_data
│   │     │     ├─ is_ph: 1 → 0  ← 转为常规CL
│   │     │     └─ head_index: X → 0  ← 清零
│   │     └─ else:
│   │         └─ 跳过（已被替换或不存在）
│   │
│   └─ printf: "[PT_CACHE] Batch updated %u entries"
│
─ 结束
```

### 3.4 Buffer链表刷新流程

```
flush_dedup_buffer_chain(task):
│
─ cur = Cache.head_index
│
├─ while (cur != 0xFF):
│   ├─ entry = Buffer[cur]
│   ├─ if !entry.is_valid(): break
│   │
│   ├─ pending_task = entry.task_ptr
│   ├─ if !pending_task: cur = entry.next_index; continue
│   │
│   ├─ 计算PA:
│   │   if task.page_sz == 4K:
│   │     pa = (task.vs_pte.PPN << 12) | (pending_task.iova & 0xFFF)
│   │   else:  // 大页
│   │     pa = task.pa  // 使用主任务的PA
│   │
│   ├─ pending_task.pa = pa
│   ├─ pt_cache_to_fwd_fifo.write(pending_task)  ← 转发任务
│   │
│   ├─ next_cur = entry.next_index
│   ├─ free_entry(cur)  ← 释放Buffer Entry
│   └─ cur = next_cur
│
└─ printf: "Flushed %u tasks from chain"
```

---

## 四、P1~P8逐包详细拆解（完整示例）

### 4.1 初始状态

```
PT Cache: 空
Buffer: [0~255] 全部 V=0, next=0xFF, tail=0xFF
```

### 4.2 P1（第1包，iova=0x1000_0000）

**步骤1**: PT Cache查询 → MISS

**步骤2**: allocate_entry() → Buffer[0]
```
Buffer[0]:
  V = 1
  Command = {gscid, pscid, iova=0x1000_0000, ...}
  next_index = 0xFF
  tail_index = 0  ← 首节点，tail=自身
  task_ptr = task0
```

**步骤3**: 创建主占位CL
```
PT Cache[0x1000_0000]:
  is_ph = 1
  head_index = 0
```

**步骤4**: 创建8个预取占位CL
```
PT Cache[0x1000_1000]: is_ph=1, head_index=0
PT Cache[0x1000_2000]: is_ph=1, head_index=0
...
PT Cache[0x1000_8000]: is_ph=1, head_index=0
```

**步骤5**: 发送PTW
```
PTW请求:
  task_id = task0
  prefetch_enabled = true
  prefetch_depth = 8
  prefetch_iovas = [0x1000_1000, ..., 0x1000_8000]
  dedup_head_index = 0
```

**状态**:
```
Buffer[0]: V=1, next=0xFF, tail=0, Command=P1
Cache: 9个占位CL (1主+8预取)，全部head_index=0
链表: [0]
```

### 4.3 P2（第2包，iova=0x1000_0200）

**步骤1**: PT Cache查询 (iova对齐到0x1000_0000)
- → 命中占位CL (is_ph=1)
- head_index = 0

**步骤2**: allocate_entry() → Buffer[1]
```
Buffer[1]:
  V = 1
  Command = {iova=0x1000_0200, ...}
  next_index = 0xFF
  tail_index = 0xFF  ← 非首节点，固定0xFF
  task_ptr = task1
```

**步骤3**: 挂接到链表
```
OldTail = Buffer[head_idx=0].tail_index = 0
Buffer[OldTail=0].next_index = 1
Buffer[head_idx=0].tail_index = 1
```

**状态**:
```
Buffer[0]: V=1, next=1, tail=1, Command=P1  ← tail更新为1
Buffer[1]: V=1, next=0xFF, tail=0xFF, Command=P2
链表: 0→1
```

### 4.4 P3~P8 快速推演

**P3** (iova=0x1000_0400):
```
Buffer[2]: V=1, next=0xFF, tail=0xFF
Buffer[1].next = 2
Buffer[0].tail = 2
链表: 0→1→2
```

**P4** (iova=0x1000_0600):
```
Buffer[3]: V=1, next=0xFF, tail=0xFF
Buffer[2].next = 3
Buffer[0].tail = 3
链表: 0→1→2→3
```

**P5** (iova=0x1000_0800):
```
Buffer[4]: V=1, next=0xFF, tail=0xFF
Buffer[3].next = 4
Buffer[0].tail = 4
链表: 0→1→2→3→4
```

**P6** (iova=0x1000_0A00):
```
Buffer[5]: V=1, next=0xFF, tail=0xFF
Buffer[4].next = 5
Buffer[0].tail = 5
链表: 0→1→2→3→4→5
```

**P7** (iova=0x1000_0C00):
```
Buffer[6]: V=1, next=0xFF, tail=0xFF
Buffer[5].next = 6
Buffer[0].tail = 6
链表: 0→1→2→3→4→5→6
```

**P8** (iova=0x1000_0E00):
```
Buffer[7]: V=1, next=0xFF, tail=0xFF
Buffer[6].next = 7
Buffer[0].tail = 7
链表: 0→1→2→3→4→5→6→7
```

### 4.5 最终Buffer状态汇总（P1~P8全部到达后）

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

## 五、PTW返回后刷新流程（精确执行）

### 5.1 前提条件

```
PTW返回结果（一次性批量返回）:
  pt_updates[0]: iova=0x1000_0000, vs_pte={PPN=0x5000, ...}
  pt_updates[1]: iova=0x1000_1000, vs_pte={PPN=0x5001, ...}
  pt_updates[2]: iova=0x1000_2000, vs_pte={PPN=0x5002, ...}
  ...
  pt_updates[8]: iova=0x1000_8000, vs_pte={PPN=0x5008, ...}
```

### 5.2 步骤1：占位CL转为常规CL（批量）

```
PT Cache接收批量更新 (is_batch_update=true, count=9):

更新主占位CL:
  iova=0x1000_0000:
    is_ph: 1 → 0
    head_index: 0 → 0
    vs_pte: {PPN=0x5000, R=1, W=1, X=1, ...}

批量更新预取占位CL:
  iova=0x1000_1000: is_ph=0, vs_pte={PPN=0x5001, ...}
  iova=0x1000_2000: is_ph=0, vs_pte={PPN=0x5002, ...}
  ...
  iova=0x1000_8000: is_ph=0, vs_pte={PPN=0x5008, ...}
```

### 5.3 步骤2：遍历Buffer链表刷新任务

```
cur = Cache.head_index = 0

循环开始:
─ Iteration 1: cur=0
│   ├─ Buffer[0].task_ptr = task0
│   ├─ 计算PA: pa = (0x5000 << 12) | (0x1000_0000 & 0xFFF) = 0x5000_0000
│   ├─ task0.pa = 0x5000_0000
│   ├─ 转发: pt_cache_to_fwd_fifo.write(task0)
│   ├─ Buffer[0].V = 0  ← 释放
│   └─ cur = Buffer[0].next_index = 1
│
├─ Iteration 2: cur=1
│   ├─ Buffer[1].task_ptr = task1
│   ├─ pa = (0x5000 << 12) | (0x1000_0200 & 0xFFF) = 0x5000_0200
│   ├─ task1.pa = 0x5000_0200
│   ├─ 转发: pt_cache_to_fwd_fifo.write(task1)
│   ├─ Buffer[1].V = 0
│   └─ cur = Buffer[1].next_index = 2
│
├─ Iteration 3: cur=2
│   ├─ task2.pa = 0x5000_0400
│   ├─ 转发task2
│   ├─ Buffer[2].V = 0
│   └─ cur = 3
│
├─ Iteration 4: cur=3
│   ├─ task3.pa = 0x5000_0600
│   ├─ 转发task3
│   ├─ Buffer[3].V = 0
│   └─ cur = 4
│
├─ Iteration 5: cur=4
│   ├─ task4.pa = 0x5000_0800
│   ├─ 转发task4
│   ├─ Buffer[4].V = 0
│   └─ cur = 5
│
├─ Iteration 6: cur=5
│   ├─ task5.pa = 0x5000_0A00
│   ├─ 转发task5
│   ├─ Buffer[5].V = 0
│   └─ cur = 6
│
─ Iteration 7: cur=6
│   ├─ task6.pa = 0x5000_0C00
│   ├─ 转发task6
│   ├─ Buffer[6].V = 0
│   └─ cur = 7
│
─ Iteration 8: cur=7
    ├─ task7.pa = 0x5000_0E00
    ├─ 转发task7
    ├─ Buffer[7].V = 0
    ─ cur = Buffer[7].next_index = 0xFF
        ─ cur == 0xFF → 循环结束

刷新完成！
```

### 5.4 最终状态

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

## 六、字段变更追踪表（P1~P8完整过程）

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

## 七、PT Cache与PTW接口确认

### 7.1 PT Cache → PTW 请求字段

**文件**: `iommu_task_t`

**必须包含的预取字段**:
```cpp
struct iommu_task_t {
    // ... 现有字段 ...
    
    // 去重相关
    uint8_t dedup_head_index = 0xFF;
    
    // 预取相关（walk_context_t中）
    struct walk_context_t {
        bool      prefetch_enabled = false;     // ← PT Cache设置
        uint32_t  prefetch_depth = 0;           // ← PT Cache设置
        iova_t    prefetch_base_iova = 0;       // ← PT Cache设置
        iova_t    prefetch_iovas[16];           // ← PT Cache设置
        // ...
    } walk_ctx;
};
```

**PT Cache设置位置** (`iommu_perf_pt_cache_response.cc` MISS分支):
```cpp
// 在发送PTW之前
task->walk_ctx.prefetch_enabled = true;
task->walk_ctx.prefetch_depth = PT_DEDUP_PREFETCH_DEPTH;  // 8
task->walk_ctx.prefetch_base_iova = task->iova;

for (uint32_t d = 0; d < PT_DEDUP_PREFETCH_DEPTH; d++) {
    task->walk_ctx.prefetch_iovas[d] = task->iova + ((d + 1) * 4096);
}

task->dedup_head_index = head_idx;

// 发送PTW
pt_cache_to_ptw_fifo.write(task);
```

### 7.2 PTW内部预取执行逻辑确认

**文件**: `iommu_perf_ptw.cc`

**需要检查的关键点**:

1. ✅ **ptw_req_process_thread**: 接收task后，检查`task->walk_ctx.prefetch_enabled`
2. ✅ **预取DDR读取**: 主任务完成后，顺序读取`prefetch_iovas[0~7]`的PTE
3. ✅ **等待所有预取完成**: 使用`PTW_PREFETCH_WAIT`状态和`prefetch_ddr_pending`计数器
4. ✅ **一次性批量返回**: 所有1+8个PTE就绪后，构造`CacheMessage.batch_updates[0~8]`，一次性写入`pt_update_fifo`

**PTW预取执行伪代码**:
```cpp
void iommu_top::ptw_req_process_thread() {
    iommu_task_t* task = ptw_req_peq.get_next_transaction();
    
    // Walk主地址
    // ... 现有walk逻辑 ...
    
    // 主任务完成后
    if (task->walk_ctx.prefetch_enabled) {
        // 启动预取DDR读取
        task->walk_ctx.prefetch_ddr_pending = task->walk_ctx.prefetch_depth;
        
        for (uint32_t d = 0; d < task->walk_ctx.prefetch_depth; d++) {
            iova_t prefetch_iova = task->walk_ctx.prefetch_iovas[d];
            uint64_t pte_addr = calculate_pte_addr(prefetch_iova);
            
            ddr_req_entry_t req;
            req.task_id = (task->task_id & 0xFFFF) | ((d + 1) << 16);
            req.addr = pte_addr;
            req.size = PTESIZE;
            ptw_req_ddr_fifo.write(req);
        }
        
        // 进入等待状态
        task->walk_ctx.walk_phase = PTW_PREFETCH_WAIT;
    }
}

void iommu_top::ptw_rsp_process_thread() {
    case PTW_PREFETCH_WAIT:
        uint32_t prefetch_idx = (rsp.task_id >> 16) & 0xFFFF;
        
        // 存储预取PTE
        task->walk_ctx.pt_updates[prefetch_idx].vs_pte = parse_pte(rsp.data);
        task->walk_ctx.prefetch_ddr_pending--;
        
        // 检查是否全部完成
        if (task->walk_ctx.prefetch_ddr_pending == 0) {
            // 一次性批量返回
            CacheMessage batch_req;
            batch_req.is_batch_update = true;
            batch_req.batch_update_count = 1 + task->walk_ctx.prefetch_depth;
            
            for (uint32_t i = 0; i < batch_req.batch_update_count; i++) {
                batch_req.batch_updates[i].iova = task->walk_ctx.pt_updates[i].iova;
                batch_req.batch_updates[i].pt_data = build_pt_data(task->walk_ctx.pt_updates[i]);
            }
            
            cache_sub.pt_update_fifo.write(batch_req);  // ← 一次性写入
            
            // 刷新Buffer链表
            flush_dedup_buffer_chain(task);
        }
        break;
}
```

---

## 八、文件修改清单

### 8.1 新建文件（2个）
1. `iommu/cache_src/common/dedup_buffer.h` - Buffer定义（含预取字段）
2. `iommu/iommu_perf_model/iommu_perf_pt_dedup_flush.cc` - 刷新逻辑

### 8.2 修改文件（11个）

**数据结构**（4个）:
1. `iommu/include/iommu_task.hh` - 扩展walk_context_t（预取字段）、iommu_task_t（dedup_head_index）
2. `iommu/cache_src/common/types.h` - 扩展pt_reserved_t（is_ph, head_index）、CacheMessage（批量更新）
3. `iommu/cache_src/common/dedup_buffer.h` - DedupBufferEntry（next_index, tail_index）
4. `iommu/cache_src/common/types.h` - 新增PTW_PREFETCH_WAIT枚举

**Cache层**（2个）:
5. `iommu/cache_src/cache/pt_cache.h` - 新增insert_placeholder, batch_update_placeholders接口
6. `iommu/cache_src/cache/pt_cache.cpp` - 实现占位CL插入、批量更新

**子系统**（1个）:
7. `iommu/cache_src/subsystem/cache_subsystem.cpp` - execute_pt_request（区分占位HIT）、execute_pt_update（批量处理）

**PTW模块**（1个）:
8. `iommu/iommu_perf_model/iommu_perf_ptw.cc` - ptw_req_process_thread（预取初始化）、ptw_rsp_process_thread（PTW_PREFETCH_WAIT状态、一次性批量返回）

**Top层**（3个）:
9. `iommu/iommu_top.hh` - 删除旧去重代码，新增DedupBuffer实例、pt_placeholder_hit_fifo
10. `iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc` - 重写collector_pt_response_thread（3种响应处理、Buffer挂接逻辑）
11. `iommu/iommu_perf_model/iommu_perf_params.hh` - 新增PT_DEDUP_PREFETCH_DEPTH参数

---

## 九、测试方案

### 9.1 单元测试

**测试1**: Buffer分配顺序
```cpp
// 验证allocate_entry()按0,1,2...顺序分配
assert(buffer.allocate_entry() == 0);
assert(buffer.allocate_entry() == 1);
assert(buffer.allocate_entry() == 2);
```

**测试2**: 链表挂接
```cpp
// P1创建链头
head = allocate_entry();  // 0
Buffer[0].tail = 0;

// P2挂接
new_idx = allocate_entry();  // 1
Buffer[0].next = 1;
Buffer[0].tail = 1;

assert(Buffer[0].next == 1);
assert(Buffer[0].tail == 1);
assert(Buffer[1].tail == 0xFF);  // 非首节点tail固定0xFF
```

**测试3**: PTW一次性返回
```cpp
// 验证PTW一次性写入batch_updates[0~8]
assert(batch_req.is_batch_update == true);
assert(batch_req.batch_update_count == 9);
for (int i = 0; i < 9; i++) {
    assert(batch_req.batch_updates[i].pt_data.reserved.is_ph == 0);
}
```

### 9.2 集成测试

**测试4**: P1~P8连续同页访问
```cpp
// 输入: P1~P8 (iova=0x1000_0000~0x1000_0E00)
// 预期:
// - PT Cache MISS: 1次 (P1)
// - PT Cache HIT (占位): 7次 (P2~P8)
// - PTW次数: 1次
// - Buffer Entry: 8个 (0~7)
// - PTW返回: 1次批量更新 (9个PTE)
// - Forwarder接收: 8个任务
```

**测试5**: 跨页连续访问（P1~P100）
```cpp
// 输入: P1~P100 (连续地址，跨越25个页)
// 预期:
// - PTW次数: 4次 (P1, P33, P65, P97)
// - PT Cache MISS: 4次
// - PT Cache HIT: 96次
// - 预取覆盖率: 87%
```

---

## 十、总结

### 10.1 8项确认要点回顾

1. ✅ **Cache无tail_index**：tail_index保存在Buffer中
2. ✅ **Buffer顺序分配**：按0、1、2...线性查找空闲Entry
3. ✅ **预取占位CL共享head**：主占位和8个预取占位的head_index全部相同
4. ✅ **刷新遍历链表**：从head开始，cur=Buffer[cur].next，直到cur=0xFF
5. ✅ **Cache占位需先申请Buffer index**：插入占位CL前先allocate_entry()
6. ✅ **PTW一次性返回1+8个PTE**：顺序读取，一次性批量返回，不分多次
7. ✅ **PT Cache→PTW请求包含预取标记**：prefetch_enabled和prefetch_depth字段
8. ✅ **PTW内部执行预取逻辑**：PTW_PREFETCH_WAIT状态等待所有预取DDR响应

### 10.2 性能预期

| 场景 | PTW次数 | Cache MISS | Buffer Entry | DDR读次数 |
|------|---------|-----------|-------------|----------|
| P1~P8（同页） | 1 | 1 | 8 | 9 |
| P1~P100（跨页，D=8） | 13 | 13 | 100 | ~205 |
| 随机访问 | 100 | 100 | 100 | ~500 |

### 10.3 实施建议

1. **第一阶段**: 扩展数据结构（pt_reserved_t, DedupBufferEntry, walk_context_t）
2. **第二阶段**: 实现Buffer分配/挂接/释放逻辑
3. **第三阶段**: 实现PT Cache占位CL插入/批量更新
4. **第四阶段**: 实现PTW预取DDR读取（PTW_PREFETCH_WAIT状态）
5. **第五阶段**: 实现一次性批量返回和flush_dedup_buffer_chain
6. **第六阶段**: 测试P1~P8场景
7. **第七阶段**: 性能验证和调优

请评审此完整确认版方案，确认无误后开始实施。
