# IOMMU 全量性能指标

## 1. 任务数与均衡性
| 设备 | injected | completed |
|---|---|---|
- Jain ALL / DATA: 1.0 / 1.0 ; 稳态吞吐 ALL 197.47 / DATA 144.35 Mreq/s

## 2. 吞吐 / 并发 / e2e / 端口间隔
| 指标 | 值 |
|---|---|
| 仿真总时长 | 72.65 us |
| IOMMU IOPS | 139.63 M trans/s |
| IOMMU 完成事务 | 10144.00 |
| IOMMU 全局并发 peak/max | 576/640 |
| IOMMU Read 并发 peak/max | 264/320 |
| IOMMU Write 并发 peak/max | 320/320 |
| IO avg e2e | 2678.1 ns |
| IO max e2e | 8732.0 ns |
| 输入端口注入平均间隔 | 5.25 ns |
| 输出端口注入平均间隔 | 6.22 ns |

## 3. Cache 命中率
| DC | 98.99% (10042/10144) |
| PC | 0.00% (0/10144) |
| PT(总) | 27.92% (2832/10144) |
| MSIPT | 96.00% (935/974) |
| Walker C2(内部) | 98.56% (274/278) |
| Walker C3(内部) | 69.08% (621/899) |
| PT 512B 数据命中率 | -0.29% |
| PT SQ/CQ/MSI 控制命中率 | 99.89% |
| Walker(VS) C3 命中率 | 69.10% (621/899) |
| Walker(VS) C2 命中率 | 30.50% (274/899) |
| S2 Walker C3 命中率 | 0.00% (1/3574) |
| S2 Walker 整体命中率 | 99.90% |

## 4. 去重 Cache / Buffer
| 指标 | 值 |
|---|---|
| 去重 Cache 命中率 | 87.71% |
| 去重 hash 冲突次数 | 0 |
|   占总查询比例 | 0.00% |
| 去重 bypass 任务数 | 0 |
|   占总数据任务比例 | 0.00% |
| 去重 Buffer Peak | 320 / 320 |
| 去重 Buffer 平均占用 | 192.99 (60.31%) |
| Buffer 满 bypass(正常预取) | 0 |
| Buffer 持有延时 avg/max/min | 1867.2 / 8101.0 / 317.0 ns |
| Hold MAIN n/W1/W2/W3 (avg) | 898 / 1735.0 / 200.0 / 7.9 ns |
| Hold MAIN max W1/W2/W3 | 7900.0 / 200.0 / 72.0 ns |
| Hold SUSP n/W1/W2/W3 (avg) | 6342 / 1609.6 / 200.0 / 7.9 ns |
| Hold SUSP max W1/W2/W3 | 7894.0 / 200.0 / 72.0 ns |

## 5. PTW
| 指标 | 值 |
|---|---|
| PTW 总任务(主+预取) | 3575 |
|   主任务 | 899 |
|     其中正常任务 | 899 |
|     其中 bypass 任务 | 0 |
|   预取任务 | 2676 |
| PTW 占 IOMMU 全部输入比 | 35.24% |
| PTW 占 IOMMU 数据任务比 | 49.05% |
| PTW 并发 peak/max | 47/64 |
| PTW 并发 平均占用(时间加权) | 21.877 |
| PTW 并发 利用率 | 34.18% |
| PTW IOPS(主+预取) | 49.21 M tasks/s |
| PTW 主任务平均 DDR 次数 | 3.62 |
| PTW 预取任务平均 DDR 次数 | 2.00 |
| PTW 执行延时 ALL 平均 | 436.22 ns |
| PTW 执行延时 MAIN 平均 | 1734.68 ns |
| PTW 任务组(1主+D预取)平均执行 | 1731.0 ns |
| PTW 注入任务平均间隔 | 77.17 ns |
| PTW 输出任务平均间隔 | 70.10 ns |
