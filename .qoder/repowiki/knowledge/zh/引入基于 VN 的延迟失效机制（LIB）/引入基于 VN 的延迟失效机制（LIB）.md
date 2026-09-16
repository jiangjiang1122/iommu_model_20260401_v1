---
kind: design
name: 引入基于 VN 的延迟失效机制（LIB）
source: session
category: adr
---

# 引入基于 VN 的延迟失效机制（LIB）

_来源：d05d581 → 1c79446 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
传统即时扫表失效在高负载下会产生大量无效扫描，浪费带宽和计算资源。需要一种延迟失效机制来减少不必要的缓存扫描操作。

## 决策驱动
- 减少无效扫描
- 共享 PT/Walker 失效状态
- 查询路径零开销
- VN 回绕批量处理

## 备选方案
- **即时扫表失效** _（已否决）_ — 优点：实现简单；缺点：高负载下产生大量无效扫描；性能退化
- **全局 VN + LIB 延迟失效** — 优点：记录待失效条目避免重复扫描；查询路径旁路比对；VN 回绕触发批量扫表；PT/Walker 共享同一 LIB

## 决策
新增 lazy_invalid_buffer.h 实现 LIB（lazy invalid buffer），包含 {V:1, C:2, TAG1:20, TAG2:16, VN:4} 条目结构。CacheSubsystem 持有全局 global_vn_（默认 4bit，VN_MAX=15）。LAZY 命令内联记录 LIB，查询路径并行查 LIB 进行旁路比对。VN 回绕时触发批量扫表，对 PT/Walker 全部 cache line 检查 CL.VN < LIB.VN 条件。任何 GLOBAL 清表同步清空 LIB 并复位 global_vn_=0。

## 影响
显著减少高负载下的无效扫描，但增加了 cache_line 存储 VN 字段的空间开销。LIB 满时降级为立即扫表保证功能正确性。查询路径在 LIB 为空时仅需一次整数判断，零开销。