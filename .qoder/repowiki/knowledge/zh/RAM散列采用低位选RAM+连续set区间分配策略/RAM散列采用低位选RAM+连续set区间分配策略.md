---
kind: design
name: RAM散列采用低位选RAM+连续set区间分配策略
source: session
category: adr
---

# RAM散列采用低位选RAM+连续set区间分配策略

_来源：ce2014b → 50bb66a 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
多RAM架构需要确定hash函数如何将org_hash映射到具体RAM和RAM内set索引，同时保证连续iova页分散到不同RAM以降低冲突。

## 决策驱动
- 连续iova页分散到不同RAM
- 跨RAM零竞争
- global_set计算简单
- num_rams为2的幂简化位运算

## 备选方案
- **高位选RAM** _（已否决）_ — 优点：均匀分布；缺点：连续iova页集中在同一RAM、增加局部热点
- **低位选RAM(org_hash & (num_rams-1))** — 优点：连续4KB页自动分散到不同RAM、位运算高效、global_set=ram_id*sets_per_ram+set_in_ram；缺点：num_rams必须为2的幂且整除num_sets

## 决策
使用org_hash & (num_rams-1)计算ram_id（默认4），org_hash >> log2(num_rams)作为RAM内set索引，每RAM独占连续set区间实现跨RAM零竞争；num_rams须为2的幂且整除num_sets。

## 影响
dedup_cache需新增configure_multi_ram/raw_hash/compute_ram_id接口；hash_function在num_rams=1时退化为旧行为；JSON配置新增dedup_cache.num_rams字段。