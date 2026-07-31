---
kind: design
name: 去重Cache采用1 Scheduler + 1 Hash + N RAM Worker多RAM并行架构
source: session
category: adr
---

# 去重Cache采用1 Scheduler + 1 Hash + N RAM Worker多RAM并行架构

_来源：490fd1f → ce2014b 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有dedup_cache为单调度线程串行处理，无法充分利用多核资源；需要支持跨RAM并发以提升吞吐，同时保持同RAM原子段串行保证一致性。

## 决策驱动
- 跨RAM零竞争提升吞吐
- 同RAM原子段串行保证一致性
- 反压传导防止死锁
- 预取任务独立调度避免阻塞

## 备选方案
- **单线程串行处理（现有）** _（已否决）_ — 优点：实现简单，无并发问题；缺点：无法利用多核，吞吐受限
- **多线程共享同一RAM** _（已否决）_ — 优点：实现相对简单；缺点：需要复杂锁机制，存在竞争热点
- **1 Scheduler + 1 Hash + N RAM Worker** — 优点：跨RAM零竞争，同RAM天然串行，反压自然传导，预取可并行；缺点：架构复杂度增加，需精心设计FIFO深度和仲裁策略

## 决策
采用1个dedup_scheduler_thread负责三入口FIFO轮询仲裁，1个dedup_hash_process_thread执行1cyc hash计算并按ram_id分发到对应RAM_FIFO，N个dedup_ram_worker_thread并行处理各自RAM的原子段操作（默认N=4）。通过`org_hash & (num_rams-1)`选择RAM号，`org_hash >> log2(num_rams)`作为RAM内set索引，实现跨RAM零竞争、同RAM串行访问。

## 影响
吞吐量理想可达1结果/cycle；但需精心设计各FIFO深度（inside_fifo=32防死锁，ram_fifo_depth=8平衡延迟与内存）；预取任务满时降级丢弃保功能；统计需按ram_id分组收集；drain逻辑需检查所有FIFO状态。