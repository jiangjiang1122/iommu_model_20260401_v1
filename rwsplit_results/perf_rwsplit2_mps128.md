# IOMMU 全量性能指标

## 1. 任务数与均衡性
| 设备 | injected | completed |
|---|---|---|
- Jain ALL / DATA: 1.0 / 1.0 ; 稳态吞吐 ALL 250.35 / DATA 229.11 Mreq/s

## 2. 吞吐 / 并发 / e2e / 端口间隔
| 指标 | 值 |
|---|---|
| 仿真总时长 | 142.69 us |
| IOMMU IOPS | 223.98 M trans/s |
| IOMMU 完成事务 | 31960.00 |
| IOMMU 全局并发 peak/max | 640/640 |
| IOMMU Read 并发 peak/max | 320/320 |
| IOMMU Write 并发 peak/max | 320/320 |
| IO avg e2e | 1250.3 ns |
| IO max e2e | 6122.5 ns |
| 输入端口注入平均间隔 | 8.13 ns |
| 输出端口注入平均间隔 | 4.25 ns |

## 3. Cache 命中率
| DC | 99.74% (31876/31960) |
| PC | 0.00% (0/31960) |
| PT(总) | 38.96% (12453/31960) |
| MSIPT | 97.23% (947/974) |
| Walker C2(内部) | 98.82% (251/254) |
| Walker C3(内部) | 74.24% (732/986) |
| PT 512B 数据命中率 | 32.98% |
| PT SQ/CQ/MSI 控制命中率 | 99.93% |
| Walker(VS) C3 命中率 | 74.20% (732/986) |
| Walker(VS) C2 命中率 | 25.50% (251/986) |
| S2 Walker C3 命中率 | 0.00% (1/3914) |
| S2 Walker 整体命中率 | 99.90% |

## 4. 去重 Cache / Buffer
| 指标 | 值 |
|---|---|
| 去重 Cache 命中率 | 94.95% |
| 去重 hash 冲突次数 | 0 |
|   占总查询比例 | 0.00% |
| 去重 bypass 任务数 | 0 |
|   占总数据任务比例 | 0.00% |
| 去重 Buffer Peak | 320 / 320 |
| 去重 Buffer 平均占用 | 112.90 (35.28%) |
| Buffer 满 bypass(正常预取) | 0 |
| Buffer 持有延时 avg/max/min | 815.3 / 5508.5 / 5.0 ns |
| Hold MAIN n/W1/W2/W3 (avg) | 982 / 1121.0 / 200.0 / 7.4 ns |
| Hold MAIN max W1/W2/W3 | 5307.5 / 200.0 / 111.5 ns |
| Hold SUSP n/W1/W2/W3 (avg) | 18443 / 585.8 / 185.0 / 7.2 ns |
| Hold SUSP max W1/W2/W3 | 5297.5 / 200.0 / 111.5 ns |

## 5. PTW
| 指标 | 值 |
|---|---|
| PTW 总任务(主+预取) | 3915 |
|   主任务 | 986 |
|     其中正常任务 | 986 |
|     其中 bypass 任务 | 0 |
|   预取任务 | 2929 |
| PTW 占 IOMMU 全部输入比 | 12.25% |
| PTW 占 IOMMU 数据任务比 | 13.45% |
| PTW 并发 peak/max | 32/64 |
| PTW 并发 平均占用(时间加权) | 7.755 |
| PTW 并发 利用率 | 12.12% |
| PTW IOPS(主+预取) | 27.44 M tasks/s |
| PTW 主任务平均 DDR 次数 | 3.33 |
| PTW 预取任务平均 DDR 次数 | 2.00 |
| PTW 执行延时 ALL 平均 | 280.14 ns |
| PTW 执行延时 MAIN 平均 | 1112.33 ns |
| PTW 任务组(1主+D预取)平均执行 | 1112.1 ns |
| PTW 注入任务平均间隔 | 141.96 ns |
| PTW 输出任务平均间隔 | 137.65 ns |
