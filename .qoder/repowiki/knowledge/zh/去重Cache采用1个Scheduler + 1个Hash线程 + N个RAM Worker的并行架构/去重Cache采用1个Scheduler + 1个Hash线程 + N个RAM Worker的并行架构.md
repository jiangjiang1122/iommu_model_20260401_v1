---
kind: design
name: 去重Cache采用1个Scheduler + 1个Hash线程 + N个RAM Worker的并行架构
source: session
category: adr
---

# 去重Cache采用1个Scheduler + 1个Hash线程 + N个RAM Worker的并行架构

_来源：ce2014b → 50bb66a 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有dedup_cache为单调度线程串行处理所有请求，无法利用多核并发；需要支持高吞吐的去重缓存以匹配PT Cache性能基线（≥122.64M IOPS）。

## 决策驱动
- 跨RAM并发提升吞吐
- 同RAM原子段串行保证一致性
- 预取任务分散到不同RAM并行执行
- 反压自然传导至入口FIFO

## 备选方案
- **单线程串行处理** _（已否决）_ — 优点：实现简单、无锁竞争；缺点：无法利用多核、吞吐受限、预取内联循环阻塞主路径
- **1 Scheduler + 1 Hash + N RAM Worker** — 优点：跨RAM完全并发、Hash单元空闲即消费、预取任务独立调度、Buffer阻塞仅影响单组RAM；缺点：需维护多FIFO、统计按ram_id分组、死锁风险需规避

## 决策
采用1个dedup_scheduler_thread轮询3个入口FIFO、1个dedup_hash_process_thread做hash计算、N=4个dedup_ram_worker_thread执行原子段操作；通过dedup_hash_in_fifo深度1实现Hash忙闲反压，RAM间零竞争。

## 影响
吞吐量从单核限制提升至4路并发；需新增dedup_inside_request_fifo(32)、dedup_hash_in_fifo(1)、dedup_ram_fifo_[0..3](8)等FIFO；统计字段按ram_id分组；drain条件需检查所有FIFO状态。