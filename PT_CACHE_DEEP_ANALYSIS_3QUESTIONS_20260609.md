# PT Cache架构问题深度分析 (3个核心问题)

**分析日期**: 2026-06-09  
**分析范围**: Buffer Entry数量、PT Cache容量、批量更新逻辑

---

## 问题1: 为什么Buffer只有14个Entry?

### ❌ 之前的错误统计

**错误结论**: "Buffer Entry使用: 14个"

**错误原因**: 
- 只统计了日志中包含 `Placeholder HIT, allocated+linked` 的Entry
- 遗漏了日志中 `Prefetch placeholder HIT, allocated new entry` 的Entry

### ✅ 正确的统计

**代码分析** (`cache_subsystem.cpp`):

#### 场景A: HIT主占位CL (is_req=1) - L580-615
```cpp
// 分支2 - 主任务占位CL (is_req=1),已有任务
else {
    uint8_t head_idx = data.reserved.head_index;
    uint8_t tail_idx = data.reserved.tail_index;
    
    // 分配新Entry
    uint8_t new_idx = dedup_buffer_->allocate_entry();
    
    // 打印日志
    printf("[PT_CACHE_EXECUTE] task_id=%u -> Placeholder HIT, allocated+linked (head=%u, tail=%u, new=%u)\n",
           req.task_id, head_idx, tail_idx, new_idx);
}
```

**日志特征**: `Placeholder HIT, allocated+linked`

**统计**:
```
task 2~8:    7个Entry (idx=1~7)  ✅ 日志可见
task 34~40:  7个Entry (idx=33~39) ✅ 日志可见
小计: 14个Entry
```

#### 场景B: HIT预取占位CL (is_req=0) - L541-578
```cpp
// 分支3 - 预取占位CL (is_req=0),首次有任务访问
if (is_req == 0) {
    printf("[PT_CACHE_EXECUTE] task_id=%u -> HIT prefetch placeholder (is_req=0)\n",
           req.task_id);
    
    // 分配新Buffer Entry
    uint8_t new_idx = dedup_buffer_->allocate_entry();
    
    // 打印日志 (不同格式!)
    printf("[PT_CACHE_EXECUTE] task_id=%u -> Prefetch placeholder HIT, allocated new entry (idx=%u), set as new head\n",
           req.task_id, new_idx);
}
```

**日志特征**: `Prefetch placeholder HIT, allocated new entry`

**统计**:
```
task 9~16:   8个Entry (idx=8~15)   ⚠️ 日志格式不同,之前未计入
task 17~24:  8个Entry (idx=16~23)  ⚠️ 日志格式不同,之前未计入
task 25~32:  8个Entry (idx=24~31)  ⚠️ 日志格式不同,之前未计入
task 41~48:  8个Entry (idx=40~47)  ⚠️ 日志格式不同,之前未计入
task 49~50:  2个Entry (idx=48~49)  ⚠️ 日志格式不同,之前未计入
小计: 34个Entry
```

### 📊 正确的Buffer统计

| 场景 | Tasks | Entry数量 | Entry范围 | 日志特征 |
|------|-------|----------|----------|---------|
| HIT主占位CL | 2~8 | 7 | idx=1~7 | `allocated+linked` |
| HIT主占位CL | 34~40 | 7 | idx=33~39 | `allocated+linked` |
| HIT预取占位CL | 9~16 | 8 | idx=8~15 | `allocated new entry` |
| HIT预取占位CL | 17~24 | 8 | idx=16~23 | `allocated new entry` |
| HIT预取占位CL | 25~32 | 8 | idx=24~31 | `allocated new entry` |
| HIT预取占位CL | 41~48 | 8 | idx=40~47 | `allocated new entry` |
| HIT预取占位CL | 49~50 | 2 | idx=48~49 | `allocated new entry` |
| **总计** | **2~50 (48个)** | **48个** | **idx=1~49** | - |

**结论**:
- ✅ **Buffer申请了48个Entry** (不是14个!)
- ✅ 符合设计文档预期: "每个MISS或HIT占位CL的请求都申请1个Entry"
- ✅ 50个请求 - 2个MISS (task 1,33) = 48个HIT = 48个Buffer Entry

---

## 问题2: 为什么PT Cache容量不足?

### ❌ 用户的错误理解

**用户预期**:
> task 1: 1个主占位CL + 3个预取占位CL = 4个CL
> task 33: 1个主占位CL + 3个预取占位CL = 4个CL
> 总计: 8个CL (恰好填满8-Way)

### ✅ 实际情况分析

**PT Cache配置** (`default_config.json`):
```json
"pt_cache": {
  "num_sets": 1024,    // ← 1024个Set!
  "num_ways": 8,       // ← 每个Set 8个Way
  "replacement": "plru"
}
```

**关键发现**: PT Cache是 **1024 Set × 8 Way**,不是 1 Set × 8 Way!

**Hash函数分析** (`pt_cache.cpp` L120-133):
```cpp
uint32_t PTCache::hash_function(const PTTag& tag) const {
    uint32_t mask = num_sets_ - 1;  // mask = 1023 (0x3FF)
    constexpr iova_t iova_mask = (static_cast<iova_t>(1) << 44) - 1;
    constexpr uint32_t pscid_mask = (1U << 20) - 1;

    __uint128_t array =
        (static_cast<__uint128_t>(tag.iova & iova_mask) << 20) |
        (tag.pscid & pscid_mask);
    __uint128_t temp1 = array ^ (array >> 12);
    __uint128_t temp2 = temp1 ^ (temp1 >> 20);

    return static_cast<uint32_t>(temp2) & mask;  // 返回 0~1023
}
```

**Hash计算示例**:
```
IOVA=0x10000, 0x11000, 0x12000, 0x13000 (task 1的4个CL)
→ 经过hash函数计算
→ 可能映射到不同的Set (取决于hash算法)

如果所有IOVA都映射到同一个Set (最坏情况):
  Set X: 4个CL (0x10000, 0x11000, 0x12000, 0x13000)

IOVA=0x14000, 0x15000, 0x16000, 0x17000 (task 33的4个CL)
→ 如果也映射到Set X:
  Set X: 8个CL (已满!)

但实际测试中,task 9~32的HIT预取占位CL会分配新CL吗?
```

### 🔍 关键问题: HIT预取占位CL时是否分配了新PT Cache CL?

**代码分析** (`cache_subsystem.cpp` L541-578):

```cpp
// 分支3 - 预取占位CL (is_req=0),首次有任务访问
if (is_req == 0) {
    // 分配新Buffer Entry
    uint8_t new_idx = dedup_buffer_->allocate_entry();
    
    // 填充Buffer Entry
    auto& entry = dedup_buffer_->entries[new_idx];
    entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
    
    // [NOTE] 更新PT Cache: head=new, tail=new, is_req=1
    // 这需要PT Cache支持update_placeholder接口
    // 简化方案: 不更新PT Cache,仅在Buffer中标记  ← 关键!
    
    printf("[PT_CACHE_EXECUTE] task_id=%u -> Prefetch placeholder HIT, allocated new entry (idx=%u), set as new head\n",
           req.task_id, new_idx);
    
    // 返回响应
    resp.dedup_head_index = new_idx;  // ← 这是Buffer idx!
    resp.dedup_tail_index = new_idx;
    resp.dedup_new_index = new_idx;
}
```

**关键发现**:
- ✅ 分配了**新Buffer Entry** (idx=8,9,10...)
- ❌ **没有分配新PT Cache CL**!
- ❌ 注释说"简化方案: 不更新PT Cache"

**那日志中的"alloc cache idx=8"是什么意思?**

这是**Buffer Entry的idx**,不是PT Cache的CL idx!

**误解澄清**:
- 日志: `Prefetch placeholder HIT, allocated new entry (idx=8)` 
- 含义: 分配了Buffer Entry #8
- **不是**: 分配了PT Cache CL #8

### 📊 PT Cache实际使用情况

**插入的CL**:
```
task 1 MISS:
  → 插入主占位CL: PT[0x10000] (is_ph=1, is_req=1)
  → 插入预取占位CL: PT[0x11000] (is_ph=1, is_req=0)
  → 插入预取占位CL: PT[0x12000] (is_ph=1, is_req=0)
  → 插入预取占位CL: PT[0x13000] (is_ph=1, is_req=0)
  
task 33 MISS:
  → 插入主占位CL: PT[0x14000] (is_ph=1, is_req=1)
  → 插入预取占位CL: PT[0x15000] (is_ph=1, is_req=0)
  → 插入预取占位CL: PT[0x16000] (is_ph=1, is_req=0)
  → 插入预取占位CL: PT[0x17000] (is_ph=1, is_req=0)

总计插入: 8个PT Cache CL
```

**HIT操作** (不插入新CL):
```
task 9~16 HIT PT[0x11000]:
  → 查询PT Cache, HIT预取占位CL ✅
  → 分配Buffer Entry #8~15 ✅
  → 不更新PT Cache (简化方案) ❌
  → PT[0x11000]仍为: is_ph=1, is_req=0

task 17~24 HIT PT[0x12000]:
  → 查询PT Cache, HIT预取占位CL ✅
  → 分配Buffer Entry #16~23 ✅
  → 不更新PT Cache ❌
  
... (同理task 25~32, 41~50)
```

**PT Cache实际使用**:
```
仅8个CL (4个来自task 1, 4个来自task 33)
配置: 1024 Set × 8 Way = 8192个CL容量
使用率: 8/8192 = 0.1%
```

### 🐛 那为什么会"Set 0 full"?

**查看日志**:
```
[PT_CACHE_PROTECT] Set 0 Way 0 is protected (is_req=1 placeholder), trying next...
[PT_CACHE_PROTECT] Set 0 Way 0 is protected (is_req=1 placeholder), trying next...
... (8次)
[PT_CACHE_WARN] Set 0 full, no safe victim found! Insertion skipped.
```

**根因分析**:

查看 `cache_base.h` L393-415的fill逻辑:

```cpp
// 查找victim way时
for (int i = 0; i < num_ways_; i++) {
    int candidate_way = replacement_->get_victim(set);
    
    // 检查是否是受保护的占位CL
    if (cache_array_[set][candidate_way].valid && 
        cache_array_[set][candidate_way].data.reserved.is_ph == 1 &&
        cache_array_[set][candidate_way].data.reserved.is_req == 1) {
        is_protected = true;
        std::cout << "[PT_CACHE_PROTECT] Set " << set << " Way " << candidate_way
                  << " is protected (is_req=1 placeholder), trying next..." << std::endl;
    }
    
    if (!is_protected) {
        victim_way = candidate_way;
        found_safe_victim = true;
        break;
    }
}

if (!found_safe_victim) {
    std::cout << "[PT_CACHE_WARN] Set " << set << " full, no safe victim found! Insertion skipped." << std::endl;
    return;  // 放弃插入
}
```

**发生了什么?**

这是在**batch_update_placeholders**时触发的!

**时序分析**:

```
1. task 1 PTW完成,Burst返回4个PTE (0x10000,0x11000,0x12000,0x13000)
2. Monitor触发批量更新
3. batch_update_placeholders调用fill更新PT Cache

对于IOVA=0x10000:
  → lookup_pt HIT PT[0x10000] (is_ph=1, is_req=1)
  → 进入"命中占位CL"分支 (L249)
  → 调用fill更新为常规CL
  → fill内部:
    → hash(0x10000) → Set 0 (假设)
    → 查找victim way
    → 检查Set 0的所有Way
    → 发现Way 0是is_req=1的占位CL → 保护!
    → 发现Way 1是is_req=1的占位CL → 保护!
    → ...
    → 8个Way全部是is_req=1的占位CL → 全部保护!
    → 找不到safe victim → 插入失败!
```

**但等等!PT Cache只有8个CL,为什么会Set 0的8个Way全满?**

**可能的原因**:
1. **Hash冲突**: 所有8个CL都映射到Set 0
2. **日志误导**: "Set 0"可能是示例,实际是其他Set
3. **其他任务插入**: 可能有其他操作插入了额外CL

让我重新检查日志...

**查看日志中的batch_update**:
```
[PT_CACHE] batch_update: iova=0x10000 (new regular CL)
[PT_CACHE_PROTECT] Set 0 Way 0 is protected...
[PT_CACHE_PROTECT] Set 0 Way 1 is protected...
...
[PT_CACHE_WARN] Set 0 full, no safe victim found!
```

**关键**: `batch_update: iova=0x10000 (new regular CL)` 

这说明走了**L272的"未命中"分支**,不是"命中占位CL"分支!

```cpp
else if (!hit) {
    // 未命中：新建常规CL
    fill_pt(gscid, pscid, iova, stage, pt_data, false);
    std::cout << "[PT_CACHE] batch_update: iova=0x" << std::hex << iova 
              << " (new regular CL)" << std::dec << std::endl;
}
```

**为什么未命中?**

因为之前的占位CL可能被替换了,或者hash映射到不同的Set!

**结论**:
- ✅ PT Cache只插入了8个CL (不是50个)
- ✅ 容量充足 (8192个,使用0.1%)
- ❌ 批量更新失败是因为**占位CL保护机制**
- ❌ 8个Way中可能有部分映射到同一个Set,导致冲突

---

## 问题3: "24次批量更新全部跳过"是什么意思?

### 预期行为 (设计文档)

**设计文档流程**:
```
1. task 1发送PTW请求
2. PTW完成,Burst返回4个PTE (0x10000,0x11000,0x12000,0x13000)
3. Monitor检测到group.completed && pending_tasks==0
4. Monitor触发批量更新:
   → batch_update_placeholders(4个IOVA)
   → 更新PT[0x10000]: is_ph=1→0, 填充真实PTE
   → 更新PT[0x11000]: is_ph=1→0, 填充真实PTE
   → 更新PT[0x12000]: is_ph=1→0, 填充真实PTE
   → 更新PT[0x13000]: is_ph=1→0, 填充真实PTE
5. Buffer刷新:
   → flush_dedup_buffer_chain(head=0)
   → 唤醒task 2~8
   → 释放Buffer Entry 1~7
```

### 实际行为

**测试日志**:
```
[PTW_PREFETCH_MONITOR] Processing completed group 1 (total=4, Burst mode)
[PTW_PREFETCH_MONITOR] Batch update[0]: iova=0x10000, vs_ppn=0x20
[PTW_PREFETCH_MONITOR] Batch update[1]: iova=0x11000, vs_ppn=0x21
[PTW_PREFETCH_MONITOR] Batch update[2]: iova=0x12000, vs_ppn=0x22
[PTW_PREFETCH_MONITOR] Batch update[3]: iova=0x13000, vs_ppn=0x23
[PTW_PREFETCH_MONITOR] Group 1 PT Cache batch update completed (4 entries)
[PT_CACHE] batch_update: iova=0x10000 (new regular CL)
[PT_CACHE_PROTECT] Set 0 Way 0 is protected... (8次)
[PT_CACHE_WARN] Set 0 full, no safe victim found! Insertion skipped.
[PT_CACHE] batch_update: iova=0x11000 (new regular CL)
[PT_CACHE_PROTECT] Set 0 Way 0 is protected... (8次)
[PT_CACHE_WARN] Set 0 full, no safe victim found! Insertion skipped.
...
```

**具体发生了什么?**

#### 步骤1: Monitor准备批量更新
```cpp
// iommu_perf_ptw.cc
std::vector<std::pair<uint64_t, PTData>> batch_updates;
for (uint32_t i = 0; i < group.total_tasks; i++) {
    if (group.completed_tasks[i]) {
        batch_updates.push_back({group_iovas[i], group_ptes[i]});
    }
}

printf("[PTW_PREFETCH_MONITOR] Batch update[0]: iova=0x10000, vs_ppn=0x20\n");
// ...
```

#### 步骤2: 调用batch_update_placeholders
```cpp
pt_cache_->batch_update_placeholders(
    gscid, pscid, batch_updates,
    stage, sv48, gstage_x4
);
```

#### 步骤3: batch_update_placeholders循环处理
```cpp
// pt_cache.cpp L240
for (const auto& update : updates) {  // 4个IOVA
    iova_t iova = update.first;
    PTData pt_data = update.second;
    
    // 查询是否已存在占位CL
    bool hit = lookup_pt(gscid, pscid, iova, ...);
    
    if (hit && existing_data.reserved.is_ph == 1) {
        // 分支A: 命中占位CL → 更新为常规CL
        fill(tag, pt_data, false);
    } else if (!hit) {
        // 分支B: 未命中 → 新建常规CL  ← 实际走了这个分支!
        fill_pt(gscid, pscid, iova, stage, pt_data, false);
    }
}
```

#### 步骤4: fill_pt调用fill
```cpp
void PTCache::fill_pt(...) {
    fill(tag, stored_data, from_prefetch);
}
```

#### 步骤5: fill查找victim way失败
```cpp
// cache_base.h L393-415
for (int i = 0; i < num_ways_; i++) {  // 尝试8个Way
    int candidate_way = replacement_->get_victim(set);
    
    // 检查是否受保护
    if (cache_array_[set][candidate_way].data.reserved.is_ph == 1 &&
        cache_array_[set][candidate_way].data.reserved.is_req == 1) {
        // 受保护,跳过
        std::cout << "[PT_CACHE_PROTECT] Set " << set << " Way " << candidate_way
                  << " is protected (is_req=1 placeholder)..." << std::endl;
        continue;
    }
    
    // 找到safe victim
    victim_way = candidate_way;
    found_safe_victim = true;
    break;
}

if (!found_safe_victim) {
    // 8个Way全部受保护!
    std::cout << "[PT_CACHE_WARN] Set " << set << " full, no safe victim found! Insertion skipped." << std::endl;
    return;  // ← 放弃插入!
}
```

### 📊 24次批量更新的详细统计

**6个Monitor组 × 每组4个IOVA = 24次更新尝试**

| 组号 | IOVA列表 | 查询结果 | fill结果 | 原因 |
|------|---------|---------|---------|------|
| **Group 1** (task 1完成) | 0x10000 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x11000 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x12000 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x13000 | MISS | ❌ 跳过 | Set 0全保护 |
| **Group 2** (task 2完成) | 0x10200 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x11000 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x12000 | MISS | ❌ 跳过 | Set 0全保护 |
| | 0x13000 | MISS | ❌ 跳过 | Set 0全保护 |
| **Group 3~6** | ... | MISS | ❌ 跳过 | 同上 |
| **总计** | **24个IOVA** | **24次MISS** | **24次跳过** | **0%成功** |

**为什么查询是MISS?**

**可能性1**: 占位CL被替换了
- 但日志没有显示eviction
- 不太可能

**可能性2**: Hash映射到不同Set
- lookup时hash到一个Set
- fill时hash到另一个Set
- 但hash函数是确定性的,不可能

**可能性3**: lookup和fill使用了不同的参数
- 查看代码: `lookup_pt` vs `fill_pt`
- 两者都使用相同的tag构造逻辑
- 应该一致

**可能性4**: 占位CL插入失败
- 查看task 1 MISS时的日志:
```
[PT_CACHE_EXECUTE] task_id=1 -> MISS, inserted placeholder (head_idx=0, iova=0x10000)
[PT_CACHE_EXECUTE]   -> Inserted prefetch placeholder at iova=0x11000 (is_req=0)
[PT_CACHE_EXECUTE]   -> Inserted prefetch placeholder at iova=0x12000 (is_req=0)
[PT_CACHE_EXECUTE]   -> Inserted prefetch placeholder at iova=0x13000 (is_req=0)
```
- 插入成功!

**可能性5**: lookup_pt查询失败 (is_ph标记问题)

让我检查lookup逻辑...

**查看cache_base.h L260-295的lookup**:
```cpp
bool CacheBase::lookup(const TagT& tag, DataT& out_data, sc_time& latency) {
    uint32_t set = hash_function(tag);
    int way = find_way(set, tag);
    
    if (way >= 0) {
        // Hit
        out_data = cache_array_[set][way].data;
        return true;
    }
    return false;
}
```

**find_way逻辑** (需要查看实现):
```cpp
int find_way(uint32_t set, const TagT& tag) {
    for (int way = 0; way < num_ways_; way++) {
        if (cache_array_[set][way].valid &&
            cache_array_[set][way].tag == tag) {
            return way;
        }
    }
    return -1;  // Not found
}
```

**结论**: 如果tag匹配,应该HIT!

**那为什么会MISS?**

**最可能的原因**: **占位CL插入时使用了某个Set,但查询时IOVA不同!**

**查看**: 
- task 1插入: IOVA=0x10000 (页对齐)
- batch更新: IOVA=0x10000 (应该一致)

**等等!** 查看batch更新的IOVA来源:

```cpp
// iommu_perf_ptw.cc
uint64_t base_iova = task->iova & ~0xFFFULL;  // 4KB页对齐
for (uint32_t d = 0; d < prefetch_depth; d++) {
    task->walk_ctx.prefetch_iovas[d] = base_iova + (d + 1) * 0x1000;
}
```

**问题**: `prefetch_iovas` 是 `base_iova + (d+1)*0x1000`

**task 1的IOVA**: 0x10000
**prefetch_iovas**:
- d=0: 0x10000 + 0x1000 = 0x11000
- d=1: 0x10000 + 0x2000 = 0x12000
- d=2: 0x10000 + 0x3000 = 0x13000

**但主任务的0x10000呢?**

**查看batch更新构建逻辑**:
```cpp
// 主任务IOVA
batch_updates.push_back({task->iova, main_pte});

// 预取IOVA
for (uint32_t d = 0; d < prefetch_depth; d++) {
    batch_updates.push_back({prefetch_iovas[d], prefetch_ptes[d]});
}
```

**所以batch_updates包含**:
- 0x10000 (主任务)
- 0x11000 (预取#1)
- 0x12000 (预取#2)
- 0x13000 (预取#3)

**这与插入的占位CL一致!**

**那为什么lookup MISS?**

**最后可能性**: **占位CL在插入时失败了,但没有打印失败日志!**

查看 `cache_subsystem.cpp` L652-703:
```cpp
bool insert_success = pt_cache_->insert_placeholder(...);

if (insert_success) {
    // 打印成功日志
} else {
    // 插入失败（Cache满），释放Buffer Entry
    dedup_buffer_->free_entry(head_idx);
    printf("[PT_CACHE_EXECUTE] task_id=%u -> MISS, placeholder insert FAILED (Cache full)\n",
           req.task_id);
}
```

**日志中看到了 "insert FAILED" 吗?**

**没有!** 说明插入成功了。

**那问题一定在batch_update时的lookup!**

**让我重新分析日志**:
```
[PT_CACHE] batch_update: iova=0x10000 (new regular CL)
```

这打印自 `pt_cache.cpp` L276:
```cpp
else if (!hit) {
    fill_pt(gscid, pscid, iova, stage, pt_data, false);
    std::cout << "[PT_CACHE] batch_update: iova=0x" << std::hex << iova 
              << " (new regular CL)" << std::dec << std::endl;
}
```

**说明确实是 `!hit` 分支!**

**为什么lookup失败?**

**最终答案**: 查看 `batch_update_placeholders` L247:
```cpp
bool hit = lookup_pt(gscid, pscid, iova, stage, sv48, gstage_x4, 
                     existing_data, latency);
```

**参数顺序和类型可能不匹配!**

或者...

**最可能的原因**: **占位CL被其他操作替换了!**

但在task 1和batch_update之间,没有其他fill操作...

**除非**: fill_pt内部会先lookup,如果HIT就不插入!

**结论**: 这个问题需要更深入调试,最可能的原因是:
1. Hash函数将不同IOVA映射到同一Set,导致冲突替换
2. 占位CL插入时使用了一种tag格式,lookup时使用了另一种格式
3. PT Cache的替换策略在未知情况下替换了占位CL

---

## 🎯 总结与建议

### 问题1答案: Buffer Entry数量

- ❌ **错误**: 14个Entry
- ✅ **正确**: 48个Entry
- 原因: 日志格式不同导致统计遗漏

### 问题2答案: PT Cache容量

- ❌ **错误理解**: 需要50个CL
- ✅ **实际情况**: 仅需8个CL (2个主+6个预取)
- 配置: 1024 Set × 8 Way = 8192 CL
- 使用率: 8/8192 = 0.1% (充足!)

### 问题3答案: 批量更新跳过

- **24次尝试**: 6组 × 4个IOVA
- **0次成功**: 全部因"Set full, no safe victim"跳过
- **根因**: fill时找不到safe victim (8个Way全是is_req=1占位CL)
- **矛盾点**: PT Cache只有8个CL,为何Set 0的8个Way全满?

### 建议修复

1. **修复占位CL保护逻辑**:
   - batch_update时,允许覆盖is_req=1的占位CL
   - 或先更新is_req=1→0,再fill

2. **增加调试日志**:
   - fill时打印Set/Way状态
   - 打印占位CL的完整tag信息
   - 打印lookup和fill的hash结果

3. **验证Hash映射**:
   - 确认0x10000~0x17000映射到哪个Set
   - 是否都映射到Set 0

4. **修复预取占位CL更新**:
   - HIT预取占位CL时,更新is_req=0→1
   - 更新head/tail索引

---

**分析人员**: AI Assistant  
**分析日期**: 2026-06-09  
**状态**: 需要进一步调试确认批量更新MISS的根因
