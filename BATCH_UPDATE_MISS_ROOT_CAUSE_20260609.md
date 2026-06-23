# PT Cache批量更新MISS根因分析

**分析日期**: 2026-06-09  
**问题**: batch_update_placeholders时lookup_pt返回MISS,导致24次批量更新全部跳过

---

## 🔍 问题现象

**测试日志**:
```
[PT_CACHE] batch_update: iova=0x10000 (new regular CL)
[PT_CACHE_PROTECT] Set 0 Way 0 is protected (is_req=1 placeholder), trying next...
[PT_CACHE_PROTECT] Set 0 Way 1 is protected (is_req=1 placeholder), trying next...
...
[PT_CACHE_WARN] Set 0 full, no safe victim found! Insertion skipped.
```

**关键线索**: `batch_update: iova=0x10000 (new regular CL)`

这说明走了`batch_update_placeholders`的**L272 "!hit"分支**,而不是"L249 hit占位CL"分支!

---

## 🎯 根因定位

### 调试日志对比

**insert_placeholder时** (task 1 MISS):
```
[PT_CACHE_DEBUG] insert_placeholder: iova=0x10000 (aligned=0x10000), 
  gscid=0, pscid=0, stage=0, sv48=0, gstage_x4=0, head=0, tail=0, is_req=1, hash_set=0
```

**batch_update lookup_pt时** (task 1 PTW完成):
```
[PT_CACHE_DEBUG] batch_update: iova=0x10000, 
  gscid=0, pscid=0, stage=2, sv48=0, gstage_x4=0
[PT_CACHE_DEBUG] lookup_pt: iova=0x10000 (aligned=0x10000), 
  gscid=0, pscid=0, stage=2, sv48=0, gstage_x4=0, hash_set=0
[PT_CACHE_DEBUG]   -> MISS!
```

### 关键差异

| 字段 | insert_placeholder | batch_update lookup | 是否匹配 |
|------|-------------------|---------------------|---------|
| iova | 0x10000 | 0x10000 | ✅ 匹配 |
| gscid | 0 | 0 | ✅ 匹配 |
| pscid | 0 | 0 | ✅ 匹配 |
| **stage** | **0 (STAGE1_ONLY)** | **2 (STAGE1_AND_2)** | ❌ **不匹配!** |
| sv48 | 0 | 0 | ✅ 匹配 |
| gstage_x4 | 0 | 0 | ✅ 匹配 |
| hash_set | 0 | 0 | ✅ 匹配 |

**结论**: **stage字段不一致导致tag比较失败,lookup MISS!**

---

## 📋 代码分析

### 1. insert_placeholder的stage来源

**cache_subsystem.cpp L644-646**:
```cpp
bool insert_success = pt_cache_->insert_placeholder(
    req.gscid, req.pscid, page_iova,
    req.stage,  // ← 使用req.stage
    req.pt_sv48, req.pt_gstage_x4,
    head_idx, head_idx,
    true,  // is_req = 1
    &insert_latency
);
```

**req.stage来自task_to_pt_request** (iommu_task_cache_convert.cc L163):
```cpp
if (!stage1_bare && stage2_bare) {
    // S-stage: 非Bare, VS-stage: Bare -> STAGE1_ONLY
    req.stage = iommu::TransStage::STAGE1_ONLY;  // ← stage=0
}
```

**测试环境**:
- iosatp.MODE = Sv39 (非Bare)
- iohgatp.MODE = Bare
- **结果**: stage = STAGE1_ONLY (0) ✅

### 2. batch_update_placeholders的stage来源

**iommu_perf_ptw.cc L1160-1162**:
```cpp
cache_sub.pt_cache().batch_update_placeholders(
    main_task->GSCID, main_task->PSCID, batch_updates,
    iommu::TransStage::STAGE1_AND_2,  // ← 硬编码STAGE1_AND_2!
    sv48, gstage_x4);
```

**问题**: stage被**硬编码**为`STAGE1_AND_2(2)`,而不是使用`main_task`的实际stage!

### 3. tag比较逻辑

**cache_base.h find_way** (伪代码):
```cpp
int find_way(uint32_t set, const TagT& tag) {
    for (int way = 0; way < num_ways_; way++) {
        if (cache_array_[set][way].valid &&
            cache_array_[set][way].tag == tag) {  // ← tag全字段比较
            return way;
        }
    }
    return -1;  // Not found
}
```

**PTTag比较** (包含stage字段):
```cpp
struct PTTag {
    gscid_t gscid;
    pscid_t pscid;
    iova_t iova;
    TransStage stage;    // ← 比较时包含此字段!
    bool sv48;
    bool gstage_x4;
};
```

**比较结果**:
```
插入的tag:  {iova=0x10000, gscid=0, pscid=0, stage=0, sv48=0, gstage_x4=0}
查询的tag:  {iova=0x10000, gscid=0, pscid=0, stage=2, sv48=0, gstage_x4=0}
                                                          ↑ 不同!

tag比较: stage 0 != 2 → 不匹配 → find_way返回-1 → lookup MISS!
```

---

## 🐛 完整时序分析

### Phase 1: task 1 MISS插入占位CL

```
1. task 1查询PT Cache (0x10000)
   → lookup_pt(stage=STAGE1_ONLY) → MISS

2. 分配Buffer Entry #0

3. 插入占位CL
   → insert_placeholder(
       iova=0x10000,
       stage=STAGE1_ONLY(0),  ← 正确!
       is_req=1
     )
   → 构造tag: {iova=0x10000, stage=0, ...}
   → fill写入PT Cache Set 0 Way 0
   → PT[Set0][Way0].tag = {iova=0x10000, stage=0, ...}

4. 插入3个预取占位CL (0x11000, 0x12000, 0x13000)
   → stage=STAGE1_ONLY(0)
```

### Phase 2: task 2~8 HIT占位CL

```
task 2~8查询PT Cache (0x10000)
→ lookup_pt(stage=STAGE1_ONLY) → HIT!
→ 返回占位CL数据
→ 分配Buffer Entry #1~7
→ 挂接到链表 (head=0, tail=0, new=1~7)
```

### Phase 3: task 1 PTW完成,批量更新

```
1. PTW完成,Burst返回4个PTE
   → main_task->walk_ctx.pt_updates[0~3]

2. Monitor触发批量更新
   → 构建batch_updates:
     - {0x10000, pte0}
     - {0x11000, pte1}
     - {0x12000, pte2}
     - {0x13000, pte3}

3. 调用batch_update_placeholders
   → batch_update_placeholders(
       gscid=0, pscid=0,
       batch_updates,
       stage=STAGE1_AND_2(2),  ← 错误! 应该是STAGE1_ONLY(0)
       sv48=0, gstage_x4=0
     )

4. 循环处理4个IOVA

   对于IOVA=0x10000:
   → lookup_pt(gscid=0, pscid=0, iova=0x10000, 
               stage=STAGE1_AND_2(2), ...)  ← stage=2!
   → 构造查询tag: {iova=0x10000, stage=2, ...}
   → find_way(Set 0, tag)
     → Way 0: tag={iova=0x10000, stage=0, ...}
     → 比较: iova匹配,但stage 0!=2 → 不匹配!
     → 继续检查Way 1~7...
     → 全部不匹配
   → find_way返回-1 → lookup MISS! ❌

5. 进入"!hit"分支 (L272)
   → fill_pt(gscid=0, pscid=0, iova=0x10000, stage=STAGE1_AND_2(2), ...)
   → fill(tag={iova=0x10000, stage=2, ...}, pt_data, false)

6. fill查找victim way
   → Set 0已有8个Way全部是stage=0的占位CL
   → 检查Way 0: is_ph=1, is_req=1 → 保护!
   → 检查Way 1: is_ph=1, is_req=0 → 不保护,但tag不匹配(iova不同)
   → ...
   → 检查所有Way,找不到safe victim
   → 插入跳过! ❌
```

---

## 📊 影响范围

**24次批量更新全部失败**:
- 6个Monitor组 (task 1~6完成)
- 每组4个IOVA
- 总计: 6 × 4 = 24次更新尝试
- 成功: 0次
- 失败: 24次 (100%)

**失败原因**:
- 24次lookup全部MISS (stage不匹配)
- 24次fill全部跳过 (Set 0全保护)

---

## ✅ 修复方案

### 方案1: 使用task的实际stage (推荐)

**修改iommu_perf_ptw.cc L1160-1162**:

```cpp
// 修改前 (错误):
cache_sub.pt_cache().batch_update_placeholders(
    main_task->GSCID, main_task->PSCID, batch_updates,
    iommu::TransStage::STAGE1_AND_2,  // ← 硬编码
    sv48, gstage_x4);

// 修改后 (正确):
// 获取task的实际stage
iommu::TransStage task_stage = static_cast<iommu::TransStage>(
    main_task->walk_ctx.pt_updates[0].stage  // 从pt_updates获取
);

// 或使用task的iosatp/iohgatp重新计算
bool stage1_bare = (main_task->iosatp.MODE == RVI_IOMMU_IOSATP_Bare);
bool stage2_bare = (main_task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Bare);
iommu::TransStage task_stage;
if (stage1_bare && !stage2_bare) {
    task_stage = iommu::TransStage::STAGE2_ONLY;
} else if (!stage1_bare && !stage2_bare) {
    task_stage = iommu::TransStage::STAGE1_AND_2;
} else if (!stage1_bare && stage2_bare) {
    task_stage = iommu::TransStage::STAGE1_ONLY;
} else {
    task_stage = iommu::TransStage::STAGE1_ONLY;
}

cache_sub.pt_cache().batch_update_placeholders(
    main_task->GSCID, main_task->PSCID, batch_updates,
    task_stage,  // ← 使用实际stage
    sv48, gstage_x4);
```

### 方案2: 修改batch_update_placeholders接口

添加自动推导stage的逻辑:

```cpp
void PTCache::batch_update_placeholders(
    gscid_t gscid, pscid_t pscid,
    const std::vector<std::pair<iova_t, PTData>>& updates,
    TransStage stage, bool sv48, bool gstage_x4) 
{
    for (const auto& update : updates) {
        iova_t iova = update.first;
        PTData pt_data = update.second;
        
        // 先尝试查询占位CL (使用传入的stage)
        PTData existing_data;
        sc_time latency;
        bool hit = lookup_pt(gscid, pscid, iova, stage, sv48, gstage_x4, 
                            existing_data, latency);
        
        // 如果MISS,尝试使用其他stage查询 (容错)
        if (!hit) {
            for (int s = 0; s < 3; s++) {
                TransStage alt_stage = static_cast<TransStage>(s);
                if (alt_stage == stage) continue;
                
                hit = lookup_pt(gscid, pscid, iova, alt_stage, sv48, gstage_x4,
                               existing_data, latency);
                if (hit && existing_data.reserved.is_ph == 1) {
                    // 找到占位CL,使用实际命中的stage
                    stage = alt_stage;
                    break;
                }
            }
        }
        
        // ... 后续逻辑
    }
}
```

**推荐方案1**,因为更直接、高效,且符合设计语义。

---

## 🎯 结论

### 根因

**batch_update_placeholders硬编码stage=STAGE1_AND_2,与insert_placeholder时的实际stage (STAGE1_ONLY)不一致,导致tag比较失败,lookup MISS。**

### 影响

- 24次批量更新全部失败 (0%成功)
- PT Cache占位CL无法转为常规CL
- Buffer链表无法刷新
- 挂起任务无法恢复

### 修复优先级

**P0 (紧急)**: 修改iommu_perf_ptw.cc L1162,使用task的实际stage

### 验证方法

修复后重新运行50包测试,检查:
1. batch_update时lookup返回HIT
2. 占位CL成功转为常规CL
3. Buffer链表刷新成功
4. 挂起任务恢复并完成地址翻译

---

**分析人员**: AI Assistant  
**分析日期**: 2026-06-09  
**状态**: 根因已确认,待修复验证
