---
kind: design
name: 预取任务从MISS内联循环改为独立任务经dedup_inside_request_fifo重新调度
source: session
category: adr
---

# 预取任务从MISS内联循环改为独立任务经dedup_inside_request_fifo重新调度

_来源：ce2014b → 50bb66a 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原方案在MISS分支内联循环执行D个预取insert，占用原子段且阻塞主路径；新架构需要将预取任务与主任务解耦，使其能hash到不同RAM并行执行。

## 决策驱动
- 预取任务可跨RAM并行
- 避免阻塞主路径原子段
- 防止inside_fifo满导致死锁
- 预取是优化非功能必需

## 备选方案
- **MISS内联循环insert预取** _（已否决）_ — 优点：逻辑简单、无需额外FIFO；缺点：阻塞主路径、无法跨RAM并行、可能引发死锁
- **独立预取任务经dedup_inside_request_fifo调度** — 优点：可hash到不同RAM并行、不阻塞主路径、scheduler优先消费防积压；缺点：需新增FIFO、满时丢弃预取任务（降级）

## 决策
在CacheMessage中新增dedup_is_prefetch标记，MISS成功时生成D个预取任务写入dedup_inside_request_fifo（iova=主iova+d*4KB），由scheduler优先消费并hash分发；inside_fifo满时使用nb_write丢弃预取任务并计数统计。

## 影响
预取吞吐显著提升但极端负载下可能丢弃预取任务；scheduler中inside_fifo优先级高于request_fifo以避免后续任务重复MISS；验证需检查预取丢弃计数=0。