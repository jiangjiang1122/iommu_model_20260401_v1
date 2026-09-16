---
kind: design
name: PT Cache 哈希函数替换为 inval_v2 模式
source: session
category: adr
---

# PT Cache 哈希函数替换为 inval_v2 模式

_来源：e917392 → a32da0e 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有 PT Cache 哈希函数对 gscid/pscid/iova 的分布不够理想，影响命中率；需要新的哈希结构以匹配 RISC-V IOMMU spec 的失效语义（temp1/temp2 分离）。

## 决策驱动
- 命中率提升
- 与失效语义对齐
- 可回退至 legacy 模式回归对比

## 备选方案
- **inval_v2 哈希（temp1=temp1^(PN>>22), temp2=PN）** — 优点：temp1 3bit 由 gscid^pscid 生成、temp2 由 PN 异或高位、set 索引更均匀、与 VMA/GVMA 小范围枚举一致
- **保留旧哈希** _（已否决）_ — 优点：无需回归验证；缺点：命中率可能退化、与失效枚举不一致

## 决策
pt_cache.cpp raw_hash/hash_function 替换为新哈希：temp1=(gscid^pscid)&0b111, temp2=PN^(PN>>22), result=((temp1<<(log2S-3))^temp2)&(S-1)；保留 legacy 路径并通过 config.pt_cache.hash_mode 切换用于回归对比。

## 影响
ram_id 计算不变、set 拆分逻辑不变；必须跑全量基线回归确认稳态 IOPS 不劣化；dedup_cache 哈希不动（去重占位与失效索引无关）。