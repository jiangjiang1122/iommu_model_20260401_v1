---
kind: design
name: Walker Cache 复用现有三级子表支持端到端大页 leaf 缓存
source: session
category: adr
---

# Walker Cache 复用现有三级子表支持端到端大页 leaf 缓存

_来源：69c282a → 2f6dfca 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
需要支持 VS/GS 两级均为 2MB 大页的两阶段翻译场景，Walker Cache 必须能缓存端到端的大页 leaf PTE（PA/SPA），命中后 PTW 可完全短路避免 DDR 访问。

## 决策驱动
- 零额外 RAM 开销
- 复用现有 C1/C2/C3 子表结构
- is_leaf 位区分中间结果与端到端 leaf 语义
- 保持 S2 路径不变

## 备选方案
- **新增独立大页 leaf 子表** _（已否决）_ — 优点：语义清晰、隔离性好；缺点：增加新的 RAM 模块、维护成本、查找路径复杂化
- **复用现有三级子表 + is_leaf 位标记** — 优点：零额外存储、lookup/update/invalidate 逻辑复用、hit_level 语义不变、S2 路径无需改动；缺点：同一子表内需通过 is_leaf 区分两种语义

## 决策
在 walker_reserved_t 中新增 is_leaf:1 位（bit6），大页 leaf 与中间结果共用现有 C1/C2/C3 子表，按页大小映射到对应级（2MB→C3、1GB→C2、512GB→C1），lookup/update/invalidate 全部复用现有 hash/多RAM 路径。

## 影响
PT Cache 对大页 leaf 始终 MISS（不写入），但 Walker Cache 可零 DDR 短路；统计新增 VS/S2 各自的 leaf-hit 计数；4KB 页路径完全保持不变。