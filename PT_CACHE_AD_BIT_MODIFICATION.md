# PT Cache A/D 位支持修改总结

## 📋 修改目标

使 PT Cache 正确保存和传递 PTE 的 A/D（Accessed/Dirty）位信息，实现以下行为：
1. **首次访问**：PTW 从 DDR 读取 PTE（A=0, D=0），触发 A/D 更新，写回 DDR（A=1, D=1），更新 PT Cache（A=1, D=1）
2. **后续访问**：PT Cache HIT 时提取 A/D 位，如果 A=1, D=1 则直接转发，**不触发 PTW 更新**

---

## 🔧 修改内容

### 1️⃣ 修改 `make_pt_data` 函数
**文件**：`iommu/cache_src/common/types.h`

**修改**：
- 添加参数 `bool ad_bit_set = true`
- 修改 A/D 位赋值逻辑，从硬编码改为使用实际值

```cpp
// 修改前
data.vs_pte.A = 1;
data.vs_pte.D = data.vs_pte.W;

// 修改后
data.vs_pte.A = ad_bit_set ? 1 : 0;
data.vs_pte.D = ad_bit_set ? data.vs_pte.W : 0;
```

---

### 2️⃣ 修改 `task_to_pt_update` 函数
**文件**：`iommu/iommu_perf_model/iommu_task_cache_convert.cc`

**修改**：
- 从 `task->vs_pte` 中提取实际 A/D 位值
- 传递给 `make_pt_data` 函数

```cpp
// [AD] 判断 A/D 位是否已设置（从 DDR 读取的实际值）
bool ad_set = (task->vs_pte.A == 1 && (!task->is_write || task->vs_pte.D == 1));

req.pt_data = iommu::make_pt_data(
    task->pa,
    iommu::PageSize::PAGE_4K,
    0x07,
    req.stage,
    iommu::PageSize::PAGE_4K,
    (req.stage == iommu::TransStage::STAGE2_ONLY) ? false : true,
    req.pt_sv48,
    req.pt_gstage_x4,
    ad_set  // [AD] A/D位实际值
);
```

**日志增强**：
```cpp
printf("[CONVERT] task_id=%u -> PT_UPDATE request (... A=%d, D=%d)\n",
       task->task_id, ..., task->vs_pte.A, task->vs_pte.D);
```

---

### 3️⃣ 修改 `pt_hit_response_to_task` 函数
**文件**：`iommu/iommu_perf_model/iommu_task_cache_convert.cc`

**修改**：
- 从 CacheMessage 中提取 A/D 位信息
- 保存到 task->vs_pte 中

```cpp
// [AD] 从 Cache 中提取 A/D 位信息
if (resp.stage == iommu::TransStage::STAGE2_ONLY) {
    task->vs_pte.A = resp.pt_data.g_pte.A;
    task->vs_pte.D = resp.pt_data.g_pte.D;
} else {
    task->vs_pte.A = resp.pt_data.vs_pte.A;
    task->vs_pte.D = resp.pt_data.vs_pte.D;
}

printf("[CONVERT] task_id=%u <- PT_LOOKUP response (HIT, pa=0x%lx, A=%d, D=%d)\n",
       task->task_id, task->pa, task->vs_pte.A, task->vs_pte.D);
```

---

### 4️⃣ 修改 PT Cache 响应处理逻辑
**文件**：`iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc`

**修改**：
- PT Cache HIT 时检查 A/D 位
- 如果 A/D 未设置且 SADE=1，送 PTW 进行硬件更新
- 如果 A/D 已设置，直接转发

```cpp
// [AD] 检查 A/D 位是否需要更新
bool need_ad_update = (task->vs_pte.A == 0 || (task->is_write && task->vs_pte.D == 0));

if (need_ad_update && task->SADE == 1) {
    // A/D 未设置且 SADE=1，需要送 PTW 进行硬件更新
    printf("[PT_CACHE] task_id=%u -> HIT but A/D update needed (A=%d, D=%d), routing to PTW\n",
           task->task_id, task->vs_pte.A, task->vs_pte.D);
    
    task->state = TASK_PTW_REQ;
    pt_cache_to_ptw_fifo.write(task);
} else {
    // A/D 已设置或 SADE=0，直接转发
    printf("[PT_CACHE] task_id=%u -> HIT, routing to fwd_fifo (pa=0x%lx, A=%d, D=%d)\n",
           task->task_id, task->pa, task->vs_pte.A, task->vs_pte.D);
    pt_cache_to_fwd_fifo.write(task);
}
```

---

## ✅ 验证结果

### 测试场景
- 2000 个请求，500 页
- VA 去重启用（87.5% hit）
- 250 个唯一页送 PTW

### 测试结果

#### 1. PT Cache 更新正确记录 A/D 位
```
[CONVERT] task_id=1 -> PT_UPDATE request (... A=1, D=1)
[CONVERT] task_id=52 -> PT_UPDATE request (... A=1, D=1)
[CONVERT] task_id=57 -> PT_UPDATE request (... A=1, D=1)
```
✅ **所有 PT_UPDATE 都正确记录了 A=1, D=1**

#### 2. PTW DDR 访问统计（保持不变）
```
Min DDR reads/task:  2  (Walker命中 + A/D更新)
Max DDR reads/task:  4  (Walker未命中 + A/D更新)
Avg DDR reads/task:  2.26
```
✅ **与修改前一致，说明逻辑正确**

#### 3. IOMMU 性能（保持不变）
```
Steady IOPS: 125.00 M trans/s
Efficiency:  100.0%
```
✅ **性能无退化**

---

## 🔍 当前限制

### PT Cache 命中率 = 0%
**原因**：PT Cache 更新晚于查询（已知的时序问题）

**影响**：
- 所有 2000 个请求都是 PT Cache MISS
- 无法验证"后续访问不触发 A/D 更新"的逻辑

**解决方案**（待实现）：
1. 预填充 PT Cache（测试前插入所有页表项）
2. 或修改测试场景（多轮访问同一批页）

---

## 📊 数据流完整示例

### 场景：首次访问 page=0x10000

```
task_id=1 (page=0x10000)
  ↓
PT Cache MISS
  ↓
PTW 读 DDR PTE (A=0, D=0)
  ↓
检测 A/D 未设置 → 触发更新
  ↓
写回 DDR (A=1, D=1)
  ↓
PT_UPDATE (A=1, D=1) → 写入 PT Cache
  ↓
task 完成
```

### 场景：后续访问 page=0x10000（期望行为，待验证）

```
task_id=X (page=0x10000)
  ↓
PT Cache HIT (A=1, D=1)
  ↓
提取 A/D 位 → A=1, D=1
  ↓
检测 A/D 已设置 → 不送 PTW
  ↓
直接转发到 forwarder
```

---

## 🎯 修改总结

| 修改项 | 状态 | 说明 |
|--------|------|------|
| `make_pt_data` 添加 A/D 参数 | ✅ | 支持传递实际 A/D 位值 |
| `task_to_pt_update` 提取 A/D | ✅ | 从 task 中读取真实值 |
| `pt_hit_response_to_task` 保存 A/D | ✅ | 从 Cache 中提取到 task |
| PT Cache 响应检查 A/D | ✅ | 根据 A/D 决定是否送 PTW |
| 日志增强 | ✅ | 打印 A/D 位信息 |
| 功能验证 | ⚠️ | PT Cache 命中率为 0，待优化测试 |

---

## 📝 后续工作

1. **优化测试场景**：使 PT Cache 能够命中，验证 A/D 位逻辑
2. **性能分析**：统计 A/D 更新对 DDR 带宽的影响
3. **扩展支持**：G-stage PTE 的 A/D 位处理（当前仅支持 VS-stage）

---

**修改日期**：2026-05-25  
**测试状态**：编译通过，功能验证中
