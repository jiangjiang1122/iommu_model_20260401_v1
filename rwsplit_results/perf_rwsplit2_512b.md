# IOMMU 全量性能指标

## 1. 任务数与均衡性
| 设备 | injected | completed |
|---|---|---|
- Jain ALL / DATA: 1.0 / 1.0 ; 稳态吞吐 ALL 250.91 / DATA 184.45 Mreq/s

## 2. 吞吐 / 并发 / e2e / 端口间隔
| 指标 | 值 |
|---|---|
| 仿真总时长 | 58.06 us |
| IOMMU IOPS | 174.72 M trans/s |
| IOMMU 完成事务 | 10144.00 |
| IOMMU 全局并发 peak/max | 640/640 |
| IOMMU Read 并发 peak/max | 320/320 |
| IOMMU Write 并发 peak/max | 320/320 |
| IO avg e2e | 1944.9 ns |
| IO max e2e | 6122.5 ns |
| 输入端口注入平均间隔 | 8.78 ns |
| 输出端口注入平均间隔 | 5.04 ns |

## 3. Cache 命中率
| DC | 99.17% (10060/10144) |
| PC | 0.00% (0/10144) |
| PT(总) | 27.84% (2824/10144) |
| MSIPT | 97.23% (947/974) |
| Walker C2(内部) | 98.91% (272/275) |
| Walker C3(内部) | 69.51% (627/902) |
| PT 512B 数据命中率 | -0.41% |
| PT SQ/CQ/MSI 控制命中率 | 99.93% |
| Walker(VS) C3 命中率 | 69.50% (627/902) |
| Walker(VS) C2 命中率 | 30.20% (272/902) |
| S2 Walker C3 命中率 | 0.00% (1/3583) |
| S2 Walker 整体命中率 | 99.90% |

## 4. 去重 Cache / Buffer
| 指标 | 值 |
|---|---|
| 去重 Cache 命中率 | 87.68% |
| 去重 hash 冲突次数 | 3 |
|   占总查询比例 | 0.04% |
| 去重 bypass 任务数 | 3 |
|   占总数据任务比例 | 0.04% |
| 去重 Buffer Peak | 320 / 320 |
| 去重 Buffer 平均占用 | 157.25 (49.14%) |
| Buffer 满 bypass(正常预取) | 0 |
| Buffer 持有延时 avg/max/min | 1208.8 / 5508.5 / 61.0 ns |
| Hold MAIN n/W1/W2/W3 (avg) | 898 / 1175.7 / 200.0 / 7.0 ns |
| Hold MAIN max W1/W2/W3 | 5307.5 / 200.0 / 74.5 ns |
| Hold SUSP n/W1/W2/W3 (avg) | 6339 / 953.0 / 200.0 / 6.9 ns |
| Hold SUSP max W1/W2/W3 | 5297.5 / 200.0 / 74.5 ns |

## 5. PTW
| 指标 | 值 |
|---|---|
| PTW 总任务(主+预取) | 3584 |
|   主任务 | 902 |
|     其中正常任务 | 899 |
|     其中 bypass 任务 | 3 |
|   预取任务 | 2682 |
| PTW 占 IOMMU 全部输入比 | 35.33% |
| PTW 占 IOMMU 数据任务比 | 49.18% |
| PTW 并发 peak/max | 51/64 |
| PTW 并发 平均占用(时间加权) | 18.536 |
| PTW 并发 利用率 | 28.96% |
| PTW IOPS(主+预取) | 61.73 M tasks/s |
| PTW 主任务平均 DDR 次数 | 3.58 |
| PTW 预取任务平均 DDR 次数 | 2.00 |
| PTW 执行延时 ALL 平均 | 293.71 ns |
| PTW 执行延时 MAIN 平均 | 1167.03 ns |
| PTW 任务组(1主+D预取)平均执行 | 1166.2 ns |
| PTW 注入任务平均间隔 | 61.26 ns |
| PTW 输出任务平均间隔 | 56.55 ns |
