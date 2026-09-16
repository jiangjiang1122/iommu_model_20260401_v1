# 性能模型 MSI 处理流程实现方案（修订版 v2）

> 本版按用户 12 点要求重写。**当前代码中已有部分先行实现（详见第 9 节差异清单），审核通过后将按本方案统一整改。**

## 0. 总体通路与分流架构

```
入口 -> Parser -> DC/PC查询(与正常请求完全一致, 点1)
  └─> Collector 获取DC后判断 (点2/点3)
       ├─ [A] S1=Bare(输入即GPA, "只开启一级翻译") 且 MSI命中
       │      → 直接分流 MSIPT 大模块
       ├─ [B] S1非Bare(单级S1-only 或 两级S1+S2)
       │      → 携带DC的 msiptp.MODE/msi_addr_mask/msi_addr_pattern(task->DC已整体携带)
       │      → 原始流程: PT Cache -> (miss) Walker Cache/去重Cache/dedup Buffer -> PTW
       │         PTW S1完成得到GPA -> MSI判断 (点4)
       │           ├─ 非MSI: 原始流程继续 (S2 -> 预取 -> 刷新 -> forward)
       │           └─ MSI:   跳过S2/预取; 刷新PT Cache(is_msi=1)/Walker Cache(is_msi=1)/
       │                     去重Cache(置无效)+刷新dedup Buffer; 任务发送 MSIPT 大模块
       └─ PT Cache HIT (点8): is_msi=1 -> 派发MSIPT; is_msi=0 -> 直接forward(写保序)

MSIPT 大模块 (点9, 与PT Cache/Walker/PTW完全独立):
  MSI Cache(GPA/msi_index -> MSI PTE解码结果, 即GPA->SPA映射)
    ├─ HIT  -> 解码输出
    └─ MISS -> MSI PTW walk(经DDR仲裁读16B MSI PTE) -> 解码 -> 回填MSI Cache
  输出判断 (点10):
    ├─ Flat/IMSIC Access -> AXI Master0 出口(与普通翻译出口一致, 写保序)
    └─ MRIF Access       -> AXI Master2 出口, 写DDR即完成
```

**MSI 识别条件（严格对齐功能模型 `iommu_msi_trans.cc` step1-5）**：`DC.msiptp.MODE != Off` 且 `(A>>12) & ~mask & mgpaw_mask == pattern & ~mask & mgpaw_mask`，命中后 `I = extract(A>>12, mask & mgpaw_mask)`。已实现的 `msi_id_check()` 保持。

**关键设计口径（待审核确认）**：
- "只开启一级地址翻译"（点2）解释为 **S1=Bare、输入地址即 GPA** 的场景（iohgatp-only），故获取 DC 后可直接用输入地址做 MSI 判断。S1 非 Bare 时（无论是否含 S2）一律在 **PTW S1 完成得到 GPA 后** 判断（与功能模型一致）。
- 点11：普通与 MSI 请求的 IOVA 段完全不重叠，因此 dedup/PT Cache 按页聚合不会混合两类请求。
- 点12：全部 MSI 逻辑以 `DC.msiptp.MODE != Off` 为守卫，`Off` 设备零新增行为。

## 1. 入口 -> DC/PC（不变，点1）

- Parser/Collector/xDTW 路径零修改；`task->DC` 已完整携带 `msiptp.MODE / msi_addr_mask / msi_addr_pattern`（点3 的携带要求天然满足，无需新增报文字段）。

## 2. Collector 获取 DC 后的分流判断（点2）

`configure_and_route()` 中，`DC.msiptp.MODE != Off` 时：
- **S1 = Bare（输入即 GPA）**：调用 `msi_id_check(task, task->iova)`，命中 → `route_to_msipt(task)` 直接进 MSIPT 大模块（不注册 `pt_cache_pending_tasks`、不查 PT/去重、不发 walker 前置）；未命中 → 正常流程。
- **S1 ≠ Bare**：不做前置判断，走原始流程；同时保持既有守卫：MSI 使能设备 **禁用预取（D=0）与 Walker 前置查询**（避免端到端 leaf 短路绕过 GPA/MSI 判断；预取禁用同时满足点7）。

## 3. 两级路径原始流程透传（点3）

- MSI 任务与普通任务一致：PT Cache 查询 → miss 后去重 Cache 占位/挂 dedup Buffer → PTW。**不再** 对 MSI 设备设置 `dedup_bypass`（撤销先行实现），同页 MSI 请求正常去重挂起（点11 保证页内全为 MSI）。
- PT Cache MISS 占位、dedup Buffer 挂起机制不改动。

## 4. PTW 内 MSI 判断与处理（点4/6/7，核心修改）

位置：`ptw_rsp_process_thread` VS leaf 完成处（`task->gpa` 生成后、S2 之前）；Bare S1 快路径同点。替换现有"立即 divert 跳过回填"的实现为：

1. `msi_id_check(task, task->gpa)` 命中后：
   - **不执行 S2**（不进 GS_EXPLICIT/S2 walker 查询）、**不发起/不等待预取**（点7；已禁用预取故无组任务）。
   - **刷新 PT Cache**：`task_to_pt_update(task)` 增加 is_msi 置位（条目保存 iova→gpa：`vs_pte.PPN = gpa页帧`，`g_pte` 不承载 S2 结果）。
   - **刷新 Walker Cache**：`task_to_walker_update(task)` 对应 cacheline is_msi=1（仅 VS 侧中间级结果，无 S2 更新）。
   - **刷新去重**（点6）：写 `dedup_update_fifo`，消息携带 MSI 标记：去重 Cache 占位 CL **置无效**（含 D 个预取占位）、**冲刷 dedup Buffer**；冲刷回调将链上全部同页任务（均为 MSI）置 `task->gpa` 后逐个 `route_to_msipt()`。
   - 主任务自身同样 `route_to_msipt()`（不进 `pt_cache_to_fwd_fifo`）。
   - PTW 资源回收（active_walks/outstanding/统计）沿用现有完成路径口径。
2. 未命中：完全走现有普通完成路径，零改动。

## 5. PT Cache / Walker Cache 新增 is_msi 字段（点5）

- `pt_reserved_t`（types.h L225，20 bit reserved）划出 1 bit `is_msi`；`make_pt_data()` 增参，`task_to_pt_update` 按 `task->is_msi` 置位。
- `walker_reserved_t`（types.h L437，恰余 1 bit reserved）划为 `is_msi`；`make_walker_data()` 增参。
- 语义：is_msi=1 表示该 cacheline 保存的是 **iova→gpa** 映射（无 S2 结果）；去重 Cache 无 is_msi 字段（点4/点6，占位置无效即可）。

## 6. PT Cache HIT 分支（点8）

`collector_pt_response_thread` 常规 CL HIT 分支：
- `resp.pt_data.reserved.is_msi == 1`：恢复 `task->gpa = (vs_pte.PPN<<12) | (iova & 0xFFF)`、`is_msi=1` → `route_to_msipt()`；
- `is_msi == 0`：现有行为 → `pt_cache_to_fwd_fifo`（普通出口，写保序不变）。
- dedup 占位 HIT（挂起任务）：由第 4 节 MSI flush 回调统一改道，无需在此处理。

## 7. MSIPT 大模块（点9，独立于 PT 通路）

组成（线程已注册，逻辑按下列口径整改）：
- **MSI Cache**：`CacheSubsystem` 内独立 `MSIPTCache`（tag = device_id + msi_index；msi_index 由 GPA 唯一导出，等价 GPA→SPA 缓存）。保存 16B MSI PTE 解码结果（`MSIPTData`：pte/pte_hi/spa/mrif_mode）。HIT 直接解码输出。
- **MSI PTW**：MISS 时经 DDR 仲裁（`DDR_SRC_MSIPTW` 已接入）读 16B MSI PTE；`msipte_decode()` 完成 V/C/M/reserved 校验并产出结果（Flat：`pa=PTE.PPN<<12|A[11:0]`；MRIF：`dest_mrif_addr/NPPN/NID`）；Flat 结果回填 MSI Cache，**MRIF 结果不回填**（功能模型依据）。
- fault（261/262/263/270/1）→ `collector_to_fault_fifo` 现有 fault 通路。

## 8. MSIPT 输出判断（点10）

`msipt_forwarder_thread` 重写：
- **Flat（IMSIC Access）**：设置翻译地址后走 **普通出口**（`reorder_mark_ready` → reorder 写保序 → Master0 语义），与正常翻译完全一致。
- **MRIF Access**：从 wdata 取 interrupt identity，计算 pending word（`mrif_addr+(iid/32)*4`、bit=`1<<(iid%32)`），经 **AXI Master2（DDR 端口）** 发出一笔 4B 写（建模 AMO_MRIF 原子 OR）；为与现有 ctrl/PTW/MSIPTW 响应配对机制隔离，DDR 仲裁新增独立源 `DDR_SRC_MSI_MRIF`（响应静默丢弃）。写发出即视为 MRIF 完成，随后回响应给发起方（经 reorder 出口）。
- notice MSI（AXI Stream）为二期可选项，一期不实现（点10 未要求）。

## 9. 与当前已实现代码的差异（整改清单）

| 项 | 现状 | 修订动作 |
|---|---|---|
| Collector 分流 | 单级/两级均按 IOVA 前置判断 | 仅 S1=Bare 前置判断；S1 非 Bare 一律走原始流程(第2节) |
| `task_to_pt_request` dedup_bypass | MSI 设备旁路 dedup | **删除**，MSI 正常参与 dedup(第3节) |
| PTW MSI 命中 | `msi_divert` 直接跳过全部回填 | 改为带 is_msi 回填 PT/Walker + dedup 失效/flush + 转 MSIPT(第4节) |
| PT/Walker is_msi | 无 | 新增字段与写回/读取逻辑(第5/6节) |
| PT HIT 路径 | 已撤销 MSI 判断 | 按 is_msi 重新加入分支(第6节) |
| msipt_forwarder | Flat/MRIF 均走 `msi_exit_direct` 独立出口 | Flat 回归普通出口；MRIF 走 Master2 写；`msi_exit_direct` 删除或仅留作 MRIF 响应辅助 |
| reorder 静默丢弃改动 | 已加 | 视出口方案保留/回退 |
| 保留项 | `msi_id_check/msipte_decode`、MGPAW 修正、MSIPT Cache 查询/回填线程、MSIPTW DDR 通道、配置检查放宽（Bare+MSIPTP 允许）、MSI 统计 | 保持 |

## 10. 测试与回归

- **场景A（S1=Bare/G-stage only，点2 直达）**：MSI 窗口直接落在输入地址空间；用例：Flat 写（pa 校验）、MRIF 写（Master2 写地址/数据校验 + DDR 内存回读）、V=0/M=0 故障（262/263）、同 vector 二次注入（MSIPT Cache HIT 计数）。
- **场景B（两级 S1+S2，点3/4/8）**：S1 将 MSI-IOVA 映射到窗口 GPA，**G-stage 不建窗口映射**（反向验证跳过 S2）；用例：首轮 PTW 识别+is_msi 回填、二轮 PT Cache HIT is_msi=1 直达 MSIPT、普通页同设备混合（验证点11/12 互扰为零）。
- **场景C（单级 S1-only）**：S1 完成后识别、无 S2，验证第 2 节口径。
- **回归**：现有场景全部 `MSIPTP_Off`，要求场景 5/6/7 稳态 IOPS 与 e2e 延时零变化（点12）。

## 11. 风险与备注

- dedup flush 回调改道 MSIPT 需要 `dedup_update` 消息携带 MSI 标记与 **gpa 页基址**（替代 `dedup_pa_base` 语义），回调按标记二选一转发——实现时优先复用 `pt_data.is_msi`，不新增消息字段。
- Walker Cache is_msi 仅影响 VS 侧条目；S2 子表条目不写 is_msi（MSI 任务不产生 S2 更新）。
- MRIF 写 DDR 走新增仲裁源后，DDR 响应静默消费，不得进入任何等待方，避免与 ctrl path 竞争。