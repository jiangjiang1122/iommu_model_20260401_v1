# IOMMU 全量性能指标

## 1. 任务数与均衡性
| 设备 | injected | completed |
|---|---|---|
- Jain ALL / DATA: 1.0 / 1.0 ; 稳态吞吐 ALL 251.02 / DATA 229.64 Mreq/s

## 2. 吞吐 / 并发 / e2e / 端口间隔
| 指标 | 值 |
|---|---|
| 仿真总时长 | 144.38 us |
| IOMMU IOPS | 221.37 M trans/s |
| IOMMU 完成事务 | 31960.00 |
| IOMMU 全局并发 peak/max | 592/640 |
| IOMMU Read 并发 peak/max | 320/320 |
| IOMMU Write 并发 peak/max | 320/320 |
| IO avg e2e | 1733.2 ns |
| IO max e2e | 6022.0 ns |
| 输入端口注入平均间隔 | 1.50 ns |
| 输出端口注入平均间隔 | 4.30 ns |

## 3. Cache 命中率
| DC | 99.73% (31875/31960) |
| PC | 0.00% (0/31960) |
| PT(总) | 9.94% (3178/31960) |
| MSIPT | 97.23% (947/974) |
| Walker C2(内部) | 98.46% (255/259) |
| Walker C3(内部) | 72.99% (700/959) |
| PT 512B 数据命中率 | 1.12% |
| PT SQ/CQ/MSI 控制命中率 | 99.89% |
| Walker(VS) C3 命中率 | 73.00% (700/959) |
| Walker(VS) C2 命中率 | 26.60% (255/959) |
| S2 Walker C3 命中率 | 0.00% (1/3808) |
| S2 Walker 整体命中率 | 99.90% |

## 4. 去重 Cache / Buffer
| 指标 | 值 |
|---|---|
| 去重 Cache 命中率 | 96.67% |
| 去重 hash 冲突次数 | 0 |
|   占总查询比例 | 0.00% |
| 去重 bypass 任务数 | 0 |
|   占总数据任务比例 | 0.00% |
| 去重 Buffer Peak | 320 / 320 |
| 去重 Buffer 平均占用 | 174.34 (54.48%) |
| Buffer 满 bypass(正常预取) | 0 |
| Buffer 持有延时 avg/max/min | 863.5 / 5506.0 / 5.0 ns |
| Hold MAIN n/W1/W2/W3 (avg) | 955 / 1134.4 / 200.0 / 9.9 ns |
| Hold MAIN max W1/W2/W3 | 5305.0 / 200.0 / 94.0 ns |
| Hold SUSP n/W1/W2/W3 (avg) | 27733 / 640.8 / 190.6 / 9.2 ns |
| Hold SUSP max W1/W2/W3 | 5295.0 / 200.0 / 94.0 ns |

## 5. PTW
| 指标 | 值 |
|---|---|
| PTW 总任务(主+预取) | 3809 |
|   主任务 | 959 |
|     其中正常任务 | 959 |
|     其中 bypass 任务 | 0 |
|   预取任务 | 2850 |
| PTW 占 IOMMU 全部输入比 | 11.92% |
| PTW 占 IOMMU 数据任务比 | 13.09% |
| PTW 并发 peak/max | 16/64 |
| PTW 并发 平均占用(时间加权) | 7.574 |
| PTW 并发 利用率 | 11.83% |
| PTW IOPS(主+预取) | 26.38 M tasks/s |
| PTW 主任务平均 DDR 次数 | 3.42 |
| PTW 预取任务平均 DDR 次数 | 2.00 |
| PTW 执行延时 ALL 平均 | 284.57 ns |
| PTW 执行延时 MAIN 平均 | 1130.28 ns |
| PTW 任务组(1主+D预取)平均执行 | 1129.4 ns |
| PTW 注入任务平均间隔 | 147.72 ns |
| PTW 输出任务平均间隔 | 143.28 ns |
