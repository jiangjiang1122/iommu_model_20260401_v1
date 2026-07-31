---
kind: design
name: Walker Cache 三级子表各自按 set 哈希分 4 组 RAM 并行 Worker
source: session
category: adr
---

# Walker Cache 三级子表各自按 set 哈希分 4 组 RAM 并行 Worker

_来源：6d4d639 → 69c282a 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
单 RAM 串行模型限制了 Walker Cache 的吞吐上限，C1/C2/C3 三级子表的查询可以并行执行。需要参照 PTCache 的多 RAM 架构对 WalkerSubCache 进行重构，提升并发度。

## 决策驱动
- 提升 Walker Cache 并发吞吐
- 与 PTCache 架构保持一致
- num_rams=1 时退化为串行保证兼容性

## 备选方案
- **每级子表独立分 4 组 RAM + Hash线程分发 + Join聚合** — 优点：三级子表可并行查询；同 RAM 内自然串行符合硬件语义；num_rams=1 时等价旧模型；per-RAM 统计便于定位瓶颈
- **全局共享 RAM 池 + 动态分配** _（已否决）_ — 优点：RAM 利用率更高；缺点：实现复杂；跨级查询协调困难；难以保证 C3>C2>C1 仲裁顺序
- **保持单 RAM 串行模型** _（已否决）_ — 优点：无需改动；缺点：吞吐受限；无法利用多核并行能力

## 决策
WalkerSubCache 新增 raw_hash()、compute_ram_id()、set 重映射及原子段接口；cache_subsystem 以 walker_hash_thread + 4 个 walker_ram_worker_thread + walker_join_thread 替换原 walker_scheduler_thread；lookup 子操作按 compute_ram_id 分发到对应 RAM Worker，join 阶段按 C3>C2>C1 仲裁选择最高优先级命中。

## 影响
新增 per-RAM 任务数/忙时/FIFO 峰值统计与 Hash 反压计数；walker_front_request_fifo 深度 16 作为前置查询入口；walker_ram_fifo_[id] 深度 ram_fifo_depth 提供反压；invalidate 聚合 affected_entries 后回写 walker_invalidate_response_fifo。JSON 配置 walker_ptw_c1/c2/c3 各加 num_rams=4, ram_fifo_depth=8。