---
name: iommu-perf-stats
description: Extracts and reports the complete IOMMU performance metric set from an iommu_model SystemC simulation log — per-device task counts & Jain fairness, IOMMU IOPS, global/read/write concurrency peaks, IO avg/max e2e, input/output port inject intervals, DC/PC/PT/MSIPT/Walker(VS)/S2-Walker cache hit rates (incl. PT 512B-data vs SQ/CQ/MSI-control split), dedup cache hit rate + hash-conflict count & ratio + bypass task count & ratio, dedup Buffer peak/avg-occupancy/full-bypass/hold-time breakdown (W1/W2/W3 for MAIN & SUSP), and PTW totals (main+prefetch, main normal vs bypass, ratio to IOMMU data tasks), PTW concurrency avg/peak/limit, PTW IOPS, main/prefetch avg DDR reads, PTW exec latency, task-group exec latency, and PTW inject/output task intervals. Use when the user asks to 统计/汇总/提取/对比 IOMMU 性能指标、性能参数、缓存命中率、PTW 或去重 Buffer 统计, or to parse an iommu_model / multidev simulation log.
---

# IOMMU 全量性能指标统计

从 `iommu_model` 仿真日志（stdout 重定向产物）一次性提取全部性能指标并计算派生比例，输出 Markdown 表（可选 JSON）。

## 何时使用

- 用户要求统计/汇总/对比 IOMMU 性能指标或性能参数。
- 用户给出一个仿真日志路径（如 `/tmp/sim_n16_static.log`、`sim_multidev_*.log`）要求出性能报告。
- 多设备（MULTIDEV_N）场景需要逐设备任务数与 Jain 公平性。

## 使用方法

运行随附脚本（只读日志尾部，数百 MB 日志也能秒级完成）：

```bash
python3 scripts/extract_iommu_perf.py <sim_log>            # 打印 Markdown 报告
python3 scripts/extract_iommu_perf.py <sim_log> --md R.md --json R.json
```

脚本位于本 skill 目录：`scripts/extract_iommu_perf.py`。在 WSL 中运行（日志通常在 WSL `/tmp`）：

```bash
wsl bash -lc "python3 /mnt/d/<proj>/.qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py /tmp/sim_xxx.log"
```

## 指标清单（脚本输出顺序）

1. **任务数与均衡性**：每设备 `injected/completed`（`[MD_RP] epoch=3`）、Jain(ALL/DATA)、稳态吞吐。
2. **吞吐/并发/e2e/端口间隔**：Sim Time、IOMMU IOPS、完成事务、全局/Read/Write 并发 peak/max、IO avg/max e2e、输入/输出端口注入平均间隔。
3. **Cache 命中率**：DC、PC、PT(总)、MSIPT、Walker C2/C3(内部表)、Walker(VS) C3/C2、S2 Walker C3 与整体；以及 **PT 512B 数据命中率** 与 **PT SQ/CQ/MSI 控制命中率** 的拆分。
4. **去重 Cache/Buffer**：去重命中率、hash 冲突次数及**占总查询比例**、bypass 任务数及**占总数据任务比例**、Buffer Peak 与平均占用、Buffer 满 bypass（正常预取）、持有延时 avg/max/min 及 MAIN/SUSP 的 W1/W2/W3 细分。
5. **PTW**：总任务(主+预取)、主任务(正常/bypass 拆分)、预取任务、**占 IOMMU 全部输入比**与**占 IOMMU 数据任务比**、并发 peak/max 与平均占用(时间加权)与利用率、PTW IOPS、主/预取平均 DDR 次数、执行延时 ALL/MAIN 平均、任务组(1主+D预取)平均执行延时、注入/输出任务平均间隔。

## 派生比例定义（脚本自动计算）

- `total_data_tasks = pt_cache.accesses − ctrl_total`（PT 总访问减去 SQ/CQ/MSI 控制包）。
- 去重冲突占比 = `dedup_conflict / dedup_lookups`。
- 去重 bypass 占比 = `dedup_bypass / total_data_tasks`。
- PT 数据命中率 = `(pt_cache.hits − ctrl_pt_hits) / total_data_tasks`。
- PTW 占数据任务比 = `ptw_completed / total_data_tasks`。
- 主任务 normal = `ptw_main − dedup_bypass`；主任务 bypass = `dedup_bypass`（hash 冲突旁路进 PTW 的主任务）。

## 注意事项

- 统计块位于日志**末尾**；脚本只读尾部 20000 行，勿对整文件 grep（数百 MB 会超时）。
- 日志需完整跑完（含 `[MULTIDEV_RESULT]`/统计段）；若被 timeout 截断，指标会显示 N/A，应先确认日志尾部。
- `Avg output interval` 在多个段重复出现，脚本按段名区分（端口段 vs PTW 段 vs PT Cache 段），勿改用全局 grep。
- 对比不同 N/参数时，对每个日志各跑一次脚本，再横向对齐同名指标。
