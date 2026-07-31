---
kind: design
name: 预取任务通过dedup_inside_request_fifo独立调度而非内联insert
source: session
category: adr
---

# 预取任务通过dedup_inside_request_fifo独立调度而非内联insert

_来源：490fd1f → ce2014b 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原方案在MISS处理中内联循环执行D个预取insert，导致主任务长时间占用调度器且无法跨RAM并行执行预取任务。

## 决策驱动
- 预取任务跨RAM并行执行
- 避免主任务阻塞调度器
- 降低PTW重复访问概率
- 简化同步复杂性

## 备选方案
- **MISS内联循环insert（现有）** _（已否决）_ — 优点：实现简单，无额外FIFO；缺点：串行执行预取，阻塞调度器，无法跨RAM并行
- **独立预取任务经dedup_inside_request_fifo重新调度** — 优点：预取可hash到不同RAM并行执行，不阻塞主任务调度，尽早占位减少后续MISS；缺点：需要新增FIFO和任务标记字段

## 决策
在CacheMessage中新增`bool dedup_is_prefetch`标记预取任务；MISS成功后按页边界裁剪D生成D个预取任务（iova=主iova+d*4KB），写入dedup_inside_request_fifo由scheduler重新调度；预取任务执行lookup→insert(is_req=0, head_index=0xFFFF)，不产生响应、不碰Buffer；inside_fifo满时使用nb_write丢弃并计数统计。

## 影响
预取效率显著提升，可减少后续PTW访问；但增加了FIFO管理和任务标记开销；极端负载下可能丢弃预取任务（性能损失非功能必需）；scheduler中inside优先策略确保预取尽早占位。