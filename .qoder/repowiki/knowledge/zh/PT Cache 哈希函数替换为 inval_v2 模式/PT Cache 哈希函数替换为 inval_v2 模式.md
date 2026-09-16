---
kind: design
name: PT Cache 哈希函数替换为 inval_v2 模式
source: session
category: adr
---

# PT Cache 哈希函数替换为 inval_v2 模式

_来源：06028a2 → d05d581 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有 PT Cache 哈希函数对 gscid/pscid/iova 的分布不够均匀，导致某些 set 冲突率高；新方案要求使用 temp1/temp2 结构的新哈希以改善命中率。

## 决策驱动
- 提升 PT Cache 命中率
- 兼容现有基线回归（legacy 模式）
- 与失效枚举扫表保持一致的 temp1 计算

## 备选方案
- **保留旧哈希函数** — 优点：无需回归验证；缺点：命中率可能劣化；与失效枚举逻辑不一致
- **直接替换为新哈希** — 优点：符合方案文档；temp1=temp1=(gscid^pscid)&0b111, temp2=PN^(PN>>22) 分布更均匀；缺点：必须跑全量基线回归确认 IOPS/命中率不劣化
- **提供可配置哈希模式** — 优点：支持 legacy 模式用于回归对比与回退；inval_v2 作为默认；缺点：增加 config 解析复杂度

## 决策
在 pt_cache.cpp 的 raw_hash/hash_function 中替换为新哈希：temp1 = (gscid ^ pscid) & 0b111, temp2 = PN ^ (PN >> 22), result = ((temp1 << (log2S - 3)) ^ temp2) & (S - 1)；通过 config `pt_cache.hash_mode: "inval_v2"(默认) | "legacy"` 控制新旧哈希切换，dedup_cache 哈希保持不变。

## 影响
ram_id 取法与 set 拆分逻辑不变；必须通过场景 5 回归确认命中率/IOPS 不劣化；新哈希与后续 VMA 小范围枚举扫表的 temp1 枚举保持一致，简化了 SCAN_RANGE 的实现。