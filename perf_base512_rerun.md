# IOMMU 全量性能指标

## 1. 任务数与均衡性
| 设备 | injected | completed |
|---|---|---|
- Jain ALL / DATA: 1.0 / 1.0 ; 稳态吞吐 ALL 250.43 / DATA 182.63 Mreq/s

## 2. 吞吐 / 并发 / e2e / 端口间隔
| 指标 | 值 |
|---|---|
| 仿真总时长 | 56.42 us |
| IOMMU IOPS | 179.78 M trans/s |
| IOMMU 完成事务 | 10144.00 |
| IOMMU 全局并发 peak/max | 513/640 |
| IOMMU Read 并发 peak/max | 211/320 |
| IOMMU Write 并发 peak/max | 320/320 |
| IO avg e2e | 2111.1 ns |
| IO max e2e | 6022.0 ns |
| 输入端口注入平均间隔 | 4.73 ns |
| 输出端口注入平均间隔 | 4.88 ns |

## 3. Cache 命中率
| DC | 99.16% (10059/10144) |
| PC | 0.00% (0/10144) |
| PT(总) | 27.92% (2832/10144) |
| MSIPT | 97.23% (947/974) |
| Walker C2(内部) | 98.56% (273/277) |
| Walker C3(内部) | 69.26% (624/901) |
| PT 512B 数据命中率 | -0.29% |
| PT SQ/CQ/MSI 控制命中率 | 99.89% |
| Walker(VS) C3 命中率 | 69.30% (624/901) |
| Walker(VS) C2 命中率 | 30.30% (273/901) |
| S2 Walker C3 命中率 | 0.00% (1/3582) |
| S2 Walker 整体命中率 | 99.90% |

## 4. 去重 Cache / Buffer
| 指标 | 值 |
|---|---|
| 去重 Cache 命中率 | 87.68% |
| 去重 hash 冲突次数 | 2 |
|   占总查询比例 | 0.03% |
| 去重 bypass 任务数 | 2 |
|   占总数据任务比例 | 0.03% |
| 去重 Buffer Peak | 320 / 320 |
| 去重 Buffer 平均占用 | 177.24 (55.39%) |
| Buffer 满 bypass(正常预取) | 0 |
| Buffer 持有延时 avg/max/min | 1324.4 / 5506.0 / 269.0 ns |
| Hold MAIN n/W1/W2/W3 (avg) | 898 / 1173.7 / 200.0 / 10.7 ns |
| Hold MAIN max W1/W2/W3 | 5305.0 / 200.0 / 59.5 ns |
| Hold SUSP n/W1/W2/W3 (avg) | 6340 / 1080.6 / 200.0 / 10.6 ns |
| Hold SUSP max W1/W2/W3 | 5295.0 / 200.0 / 59.5 ns |

## 5. PTW
| 指标 | 值 |
|---|---|
| PTW 总任务(主+预取) | 3583 |
|   主任务 | 901 |
|     其中正常任务 | 899 |
|     其中 bypass 任务 | 2 |
|   预取任务 | 2682 |
| PTW 占 IOMMU 全部输入比 | 35.32% |
| PTW 占 IOMMU 数据任务比 | 49.16% |
| PTW 并发 peak/max | 47/64 |
| PTW 并发 平均占用(时间加权) | 19.133 |
| PTW 并发 利用率 | 29.90% |
| PTW IOPS(主+预取) | 63.50 M tasks/s |
| PTW 主任务平均 DDR 次数 | 3.61 |
| PTW 预取任务平均 DDR 次数 | 2.00 |
| PTW 执行延时 ALL 平均 | 294.56 ns |
| PTW 执行延时 MAIN 平均 | 1171.38 ns |
| PTW 任务组(1主+D预取)平均执行 | 1168.9 ns |
| PTW 注入任务平均间隔 | 59.52 ns |
| PTW 输出任务平均间隔 | 54.80 ns |
