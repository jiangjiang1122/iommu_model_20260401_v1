---
kind: design
name: PT Cache 采用多 RAM 并行架构替代单 RAM 串行流水
source: session
category: adr
---

# PT Cache 采用多 RAM 并行架构替代单 RAM 串行流水

_来源：a5f8a19 → 94279e3 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有 pt_scheduler_thread 为单 RAM 串行流水线，一个任务全部执行完才读下一个，吞吐受限。需要提升 PT Cache 的并发处理能力以匹配更高 IOPS 需求。

## 决策驱动
- 提升吞吐能力（IOPS）
- 保持向后兼容（num_rams=1 退化）
- 避免跨 RAM 数据竞争
- 保留现有延时建模规范

## 备选方案
- **多 RAM + Hash 分发 + 独立 worker 线程** — 优点：跨 RAM 完全并发、无锁设计、Hash 单元反压控制流量、每 RAM 独占 set 区间避免竞争
- **共享内存 + 锁仲裁** _（已否决）_ — 优点：实现简单；缺点：锁竞争成为新瓶颈、时序难以建模、性能提升有限

## 决策
在 CacheSubsystem 层引入 Hash 单元与 num_rams 个 pt_ram_worker_thread，通过 hash&(num_rams-1) 将请求路由到对应 RAM 的 FIFO，每个 RAM 独立串行处理其 set 区间内的所有操作，跨 RAM 零竞争。

## 影响
吞吐量预期持平或提升；需维护 num_rams 配置与 JSON 解析；统计模块需按 RAM 分组输出利用率；gap 分析在并发下语义变弱但保留；向后兼容 num_rams=1 时行为不变。