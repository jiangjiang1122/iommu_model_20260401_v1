---
kind: design
name: PT/Walker 共享 LIB/VN 延迟失效机制
source: session
category: adr
---

# PT/Walker 共享 LIB/VN 延迟失效机制

_来源：1c79446 → 1a81109 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
VMA/GVMA 扫表类失效（仅 GSCID/PSCID）若立即扫描整个 PT/Walker 缓存代价高昂，且规范允许延迟失效。需要在查询路径旁路比对 VN，避免每次扫表的 O(n) 开销。

## 决策驱动
- 降低扫表类失效的写放大
- 查询路径零开销（LIB 为空时短路）
- PT/Walker 共享同一 PTE 失效语义
- VN 回绕时批量扫表

## 备选方案
- **全局 VN + LIB 结构体 + 查询旁路 CAM 比对** — 优点：LIB 为空时 O(1) 短路、VN 回绕触发批量扫表、PT/Walker 共享语义一致、dedup/预取不受影响
- **直接扫表失效** _（已否决）_ — 优点：实现简单；缺点：GSCID/PSCID 级扫表代价高、无法利用未使用的缓存行、不符合 spec 延迟语义
- **每个 Cache 独立 LIB** _（已否决）_ — 优点：解耦；缺点：同一 PTE 失效需分别通知、VN 不同步导致语义错误

## 决策
CacheSubsystem 持有全局 global_vn_（默认 4bit），PT 与 Walker 共享同一 LIB（默认 16 条目）和 VN；CacheLine 增加 vn 字段，fill/insert 时记录当前 VN；LAZY 命令内联更新 LIB 并 VN++；查询路径命中后并行查 LIB（建模 1~2 拍 CAM），CL.VN < LIB.VN 则按 miss 返回；VN 回绕时触发批量扫表。

## 影响
查询路径仅在 LIB 非空时增加一次整数判断+CAM 查找；VN_MAX=15 时触发全缓存批量扫表；任何 GLOBAL 清表联动清空 LIB 并复位 VN；dedup 占位与 Buffer 链不受失效影响，天然满足失效后写入数据保留语义。