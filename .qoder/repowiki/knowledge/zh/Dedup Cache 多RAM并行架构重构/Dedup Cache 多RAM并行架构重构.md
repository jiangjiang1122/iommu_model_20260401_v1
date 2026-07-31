---
kind: design
name: Dedup Cache 多RAM并行架构重构
source: session
category: adr
---

# Dedup Cache 多RAM并行架构重构

_来源：94279e3 → 490fd1f 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原 dedup_cache 采用单线程串行处理（dedup_scheduler_thread 同时完成 hash、lookup/insert、buffer管理、响应路由），每任务固定消耗 DEDUP_ATOMIC_CYCLES=5cyc，无法利用多RAM并发，稳态吞吐受限于 1 task/5cyc。需要参照 PT Cache 的多RAM模式进行重构以提升吞吐。

## 决策驱动
- 跨RAM并发提升吞吐
- 避免Buffer操作阻塞RAM Worker
- 与PT Cache架构保持一致性
- 保持功能正确性

## 备选方案
- **单线程串行（现状）** _（已否决）_ — 优点：实现简单，无跨线程同步问题；缺点：无法利用多RAM并发，吞吐受限为1 task/5cyc
- **Scheduler + Hash + N RAM Worker 三阶段流水线** — 优点：Hash单元独立、N个RAM Worker跨RAM并发、Buffer操作在Scheduler中执行不阻塞RAM；缺点：增加FIFO通信开销、调度复杂度上升
- **所有操作都在RAM Worker内完成（含Buffer）** _（已否决）_ — 优点：简化线程间数据传递；缺点：Buffer满时wait(free_event)会阻塞整组RAM Worker，降低并发度

## 决策
采用 Scheduler + Hash + N RAM Worker 的三阶段架构：dedup_scheduler_thread 负责入口仲裁和Buffer管理；dedup_hash_process_thread 执行hash并分发到per-RAM FIFO；N个 dedup_ram_worker_thread 各自处理对应RAM的原子段操作。num_rams 复用 pt_cache.num_rams 配置，默认4组RAM。预取任务通过新增的 dedup_inside_request_fifo 重新进入调度队列，经hash分散到不同RAM并行执行。

## 影响
吞吐从 ~1 task/5cyc 提升到 ~N tasks/5cyc（理想情况）。新增 dedup_lookup_rsp_fifo 和 dedup_update_rsp_fifo 两个响应通道，以及 DedupRamResult 结构体承载结果。HIT预取占位的升级操作需要额外一次modify请求到RAM Worker。死锁规避：inside_fifo深度32且scheduler优先消费，预取写入使用nb_write满则丢弃。统计维度扩展到每个RAM的任务数、忙时、FIFO峰值等。