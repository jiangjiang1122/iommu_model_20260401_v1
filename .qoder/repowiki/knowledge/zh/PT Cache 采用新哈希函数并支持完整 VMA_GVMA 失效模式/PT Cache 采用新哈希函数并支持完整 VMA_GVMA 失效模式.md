---
kind: design
name: PT Cache 采用新哈希函数并支持完整 VMA/GVMA 失效模式
source: session
category: adr
---

# PT Cache 采用新哈希函数并支持完整 VMA/GVMA 失效模式

_来源：d05d581 → 1c79446 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有 PT Cache 哈希函数无法支持小范围枚举扫表等复杂失效场景，且 VMA/GVMA 仅实现了部分模式，不符合 RISC-V IOMMU V1.0.1 spec 要求的 8 种 VMA 模式和 4 种 GVMA 模式。

## 决策驱动
- spec 合规性
- 小范围枚举扫表支持
- 回归兼容性
- NL 非叶失效语义

## 备选方案
- **保持旧哈希函数** _（已否决）_ — 优点：无需改动现有逻辑；缺点：无法支持 temp1/temp2 结构的小范围枚举；不满足 spec 要求
- **新哈希函数 + legacy 开关** — 优点：支持完整的 temp1/temp2 枚举结构；保留 legacy 模式用于回归对比；向后兼容

## 决策
替换 pt_cache.cpp 中 raw_hash/hash_function 为新哈希：temp1 = (gscid ^ pscid) & 0b111, temp2 = PN ^ (PN >> 22), result = ((temp1 << (log2S - 3)) ^ temp2) & (S - 1)。新增 pt_cache.hash_mode: 'inval_v2'(默认) | 'legacy' 配置项。实现完整的 VMA 8 模式（GLOBAL/LAZY/SCAN_RANGE/PRECISE）和 GVMA 4 模式，支持 NL 非叶失效语义联动 Walker Cache。

## 影响
新哈希函数支持更复杂的失效场景，但必须跑全量基线回归比对稳态 IOPS。查询路径增加 LIB/VN 旁路比对，LIB 为空时 O(1) 短路保证零开销。