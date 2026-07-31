---
kind: design
name: dedup_cache配置独立于pt_cache并参数化多RAM几何
source: session
category: adr
---

# dedup_cache配置独立于pt_cache并参数化多RAM几何

_来源：490fd1f → ce2014b 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
原有dedup_cache复用pt_cache配置，无法独立调优RAM数量和FIFO深度等关键参数。

## 决策驱动
- 独立调优去重缓存几何
- 支持不同RAM数量配置
- 向后兼容单RAM模式
- JSON配置驱动

## 备选方案
- **复用pt_cache配置（现有）** _（已否决）_ — 优点：配置简单；缺点：无法独立优化dedup_cache几何
- **独立dedup_cache配置段** — 优点：可独立配置num_rams、ram_fifo_depth等参数，支持num_rams=1退化为旧行为；缺点：配置结构更复杂

## 决策
在GlobalConfig中新增`CacheConfig dedup_cache`段，包含num_rams（默认4）、ram_fifo_depth（默认8）、num_sets/num_ways（默认与pt_cache相同）；JSON解析新增dedup_cache段；hash_function支持num_rams=1时退化为旧单RAM行为；num_rams须为2的幂且整除num_sets。

## 影响
支持灵活配置不同规模的multi-RAM部署；向后兼容单RAM场景；配置验证需在json_config.cpp中增加约束检查；默认几何与pt_cache保持一致便于测试对比。