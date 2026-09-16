---
kind: design
name: PT/Walker Cache 引入基于 VN 的延迟失效（LIB）机制
source: session
category: adr
---

# PT/Walker Cache 引入基于 VN 的延迟失效（LIB）机制

_来源：e917392 → a32da0e 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
传统扫表类失效需遍历全部 cache line，延时长且浪费带宽；需要一种能推迟失效、仅在查询时比对并支持批量扫表的机制来优化性能。

## 决策驱动
- 减少扫表延时
- 查询路径零开销短路
- VN 回绕批量扫表
- 与 dedup/预取协同

## 备选方案
- **全局 VN + LIB 延迟失效** — 优点：LAZY 命令内联 1~2 拍完成、查询旁路 CAM 并行不额外延时、VN 回绕触发批量扫表、LIB 满降级立即扫表保功能正确
- **直接扫表失效** _（已否决）_ — 优点：实现简单；缺点：每次扫表遍历全部 cache line、延时长、浪费带宽

## 决策
在 CacheLine 增加 vn 字段，CacheSubsystem 持有 global_vn_（默认 4bit），PT 与 Walker 共享同一 LIB 与 VN；LAZY 命令记录 (C,TAG1,TAG2,VN) 到 LIB，查询命中后并行查 LIB 比对 CL.VN < LIB.VN 判定失效；global_vn_ 回绕时广播 SCAN 子任务批量扫表。

## 影响
查询路径在 LIB 为空时 O(1) 短路（基线零开销）；GLOBAL 清表联动清空 LIB 并复位 VN；dedup 占位与 Buffer 链不受影响（新刷入 CL 记录当前 VN 天然满足语义）；VN_MAX=15 时回绕触发全量扫表。