---
kind: design
name: 采用命令驱动的延迟失效（LIB+VN）替代实时扫表
source: session
category: adr
---

# 采用命令驱动的延迟失效（LIB+VN）替代实时扫表

_来源：06028a2 → d05d581 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
RISC-V IOMMU V1.0.1 规范要求支持 LAZY 失效语义，但传统实时扫表在 GSCID/PSCID 范围失效时会导致性能退化；同时 PT/Walker 共享 PTE 需要一致的失效可见性。

## 决策驱动
- 零热路径开销（无失效命令时 O(1) 短路）
- PT/Walker 共享失效状态的一致性
- VN 回绕时的批量扫表效率
- 与现有基线场景的回归兼容性

## 备选方案
- **实时扫表失效** _（已否决）_ — 优点：实现简单、无需额外数据结构；缺点：GSCID/PSCID 范围失效时扫描全部 cache line 导致性能退化；无法利用 VN 优化
- **基于 LIB+VN 的延迟失效** — 优点：LAZY 命令仅记录标签（1~2 拍），查询路径旁路比对仅在 LIB 非空时执行；VN 回绕触发批量扫表；PT/Walker 共享同一缓冲保证一致性；缺点：需扩展 CacheLine 增加 vn 字段；LIB 满时需降级为立即扫表；VN 回绕逻辑复杂
- **独立失效队列 + 后台线程处理** — 优点：与查询路径完全解耦；缺点：引入额外线程调度开销；难以保证 IOFENCE 语义的精确同步

## 决策
在 CacheSubsystem 中维护全局 4bit VN 计数器与可配大小的 LIB（默认 16 项），PT 与 Walker 共享同一 LIB/VN；LAZY 命令在 hash 线程内联写入 LIB 并立即响应，查询路径命中后并行查 LIB 比对 CL.VN < LIB.VN 判定失效；VN 达到 VN_MAX 时触发全量 cache scan 批量失效。

## 影响
查询路径在 LIB 为空时保持零开销（仅一次整数判断）；VN 回绕时批量扫表会短暂阻塞 RAM worker 但真实硬件亦如此；功能级 ddt_cache/pdt_cache/tlb 仍由原有逻辑兜底，性能模型只影响性能级 Cache 阵列；新增 `invalidation` 配置段控制 lazy_enable/lib_size/vn_bits 等参数。