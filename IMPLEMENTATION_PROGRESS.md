# PT Cache 去重+预取方案实施进度

## 已完成阶段

### ✅ 第一阶段：扩展数据结构（100%完成）

**修改文件**：
1. ✅ `iommu/cache_src/common/types.h`
   - pt_reserved_t: 添加is_ph(1bit)和head_index(8bit)
   - CacheMessage: 添加批量更新字段（is_batch_update, batch_update_count, batch_updates[17]）

2. ✅ `iommu/include/iommu_task.hh`
   - ptw_walk_phase_t: 添加PTW_PREFETCH_WAIT枚举
   - walk_context_t: 添加预取字段（prefetch_enabled, prefetch_depth, prefetch_iovas[16], pt_updates[17], prefetch_ddr_pending）
   - iommu_task_t: 添加dedup_head_index字段

3. ✅ `iommu/iommu_perf_model/iommu_perf_params.hh`
   - 添加PT_CACHE_DEDUP_ENABLED, PT_DEDUP_BUFFER_SIZE, PT_DEDUP_PREFETCH_DEPTH, DEDUP_BUFFER_INVALID_IDX参数

4. ✅ `iommu/cache_src/common/dedup_buffer.h` (新建)
   - DedupBufferEntry结构
   - DedupBuffer管理类（allocate_entry, free_entry）

---

## 待完成阶段

### ⏳ 第二阶段：实现Buffer管理逻辑（0%）

**需要完成**：
1. 在`iommu_top.hh`中添加DedupBuffer实例
2. 在collector_pt_response_thread中实现Buffer挂接逻辑（占位CL命中分支）
3. 创建链头逻辑（MISS分支）

### ⏳ 第三阶段：实现PT Cache占位CL操作（0%）

**需要完成**：
1. 在`pt_cache.h`中添加接口：
   - `insert_placeholder()`
   - `batch_update_placeholders()`
2. 在`pt_cache.cpp`中实现占位CL插入和批量更新逻辑
3. 修改`execute_pt_request()`：区分3种响应（常规CL命中、占位CL命中、MISS）

### ⏳ 第四阶段：实现PTW预取机制（0%）

**需要完成**：
1. 在`iommu_perf_ptw.cc`中修改`ptw_req_process_thread()`：
   - 检查prefetch_enabled
   - 计算预取IOVA序列
   - 设置prefetch_iovas
2. 修改`ptw_rsp_process_thread()`：
   - 添加PTW_PREFETCH_WAIT状态处理
   - 顺序读取D个预取PTE
   - 等待所有预取完成（prefetch_ddr_pending==0）
   - 构造批量更新消息（is_batch_update=true）
   - 一次性写入pt_update_fifo

### ⏳ 第五阶段：实现Buffer刷新逻辑（0%）

**需要完成**：
1. 创建`iommu_perf_pt_dedup_flush.cc`（新建）
2. 实现`flush_dedup_buffer_chain()`函数：
   - 从Cache.head_index开始遍历
   - 计算PA
   - 转发task到forwarder
   - 释放Buffer Entry
3. 在PTW批量更新后调用flush_dedup_buffer_chain()

### ⏳ 第六阶段：编译并单元测试（0%）

**需要完成**：
1. 编译所有修改文件
2. 修复编译错误
3. 运行单元测试：
   - Buffer分配顺序测试
   - 链表挂接测试
   - PTW一次性返回测试

### ⏳ 第七阶段：1000包回归测试+性能统计（0%）

**需要完成**：
1. 配置测试参数（PT_DEDUP_PREFETCH_DEPTH=8）
2. 运行1000包请求测试
3. 统计性能指标：
   - Cache命中率
   - PTW执行任务数目
   - DDR读次数
   - 去重效果

---

## 实施建议

由于代码修改量较大，建议：

1. **先完成核心逻辑**：Buffer管理 + PT Cache占位CL + PTW预取
2. **逐步编译测试**：每完成一个阶段就编译验证
3. **最后集成测试**：所有模块完成后进行1000包回归测试

---

## 下一步行动

请选择：
- **A. 继续实施**：我将按阶段继续完成代码修改
- **B. 先编译当前代码**：检查第一阶段修改是否有编译错误
- **C. 创建简化版本**：先实现基础去重（不含预取），后续再加预取功能

请告知您的选择，我将继续实施。
