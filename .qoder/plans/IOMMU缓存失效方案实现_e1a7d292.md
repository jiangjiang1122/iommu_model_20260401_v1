# IOMMU 缓存失效方案实现（DC/PC/PT/Walker + CQ）

## Summary

基于 `riscv-iommu.pdf`（V1.0.1 软件指南）与本方案文档，在现有 SystemC 性能模型中实现四类 Cache 的失效处理：

1. **CQ 通路打通**：功能模型 CQ（`iommu/iommu_perf_model/iommu_command_queue.cc`）解析出的 IODIR/IOTINVAL/IOFENCE 命令，桥接到 `CacheSubsystem` 的失效 pipeline（`invalidation_request_fifo`）。
2. **调度进程改造**：DC/PT/Walker 的调度进程将失效 FIFO 纳入统一调度，且失效任务优先级最高（不再轮询/乒乓平级）。
3. **DC/PC 失效**：INVAL_DDT 按 DV 精准/全清 DC，并由硬件关联失效 PC（替代原 PT/Walker/MSI 级联，符合 spec V1.0.1）；INVAL_PDT 按 DID+PID 精准失效 PC。
4. **PT Cache 失效**：新哈希函数（temp1/temp2 结构）、VMA/GVMA 全部 8/4 种模式（清表/扫表/小范围枚举扫表/精准）、NL 非叶失效语义。
5. **延迟失效**：全局 4bit VN + LIB（lazy invalid buffer），扫表类失效（仅 GSCID/PSCID）记录进 LIB，查询路径旁路比对 VN，VN 回绕时批量扫表。
6. **Walker Cache 失效**：与 PT Cache 协同，支持小范围枚举失效与 NL 语义，共享同一 LIB/VN。

**基线保护原则**：场景 5/6/7/8 不发失效命令 → 除 PT 哈希函数替换外所有新逻辑均为"命令驱动、零热路径开销"；查询路径的 LIB 比对在 LIB 为空时 O(1) 短路；哈希替换后必须跑全量基线回归比对稳态 IOPS。

---

## 一、CQ 命令队列通路（iommu_command_queue + iommu_top）

- **命令格式扩展**（`iommu/include/iommu_command_queue.hh`）：`command_t` 已含 iotinval（gv/av/nl/pscv/s/gscid/pscid/addr）与 iodir（dv/did/pid）全部字段，无需改位域；确认 NL 位（bit34）与 S 位（bit73）解析正确即可。
- **桥接机制**：在 `iommu_t`（或经全局注册函数）新增失效桥接回调指针 `perf_inval_hook(const InvalidationCmd&)`，由 `iommu_top` 构造时注册。`do_inval_ddt / do_inval_pdt / do_iotinval_vma / do_iotinval_gvma` 在保留原功能级失效（ddt_cache/pdt_cache/tlb）的同时，构造 `CacheMessage`（cmd_type/gv/av/nl/pscv/dv/gscid/pscid/did/pid/addr）调用 hook。
- **iommu_top 侧 CQ 管理**：
  - 新增 `SC_THREAD cq_process_thread`：每拍轮询（cqh != cqt 时）调用 `process_commands()`，即"CQ 命令队列进程"；空闲时 wait 在 cqt doorbell 事件或周期轮询。
  - hook 将失效命令写入 `cache_subsystem.invalidation_request_fifo`，并递增 `pending_inval_count_`；`invalidation_response_fifo` 由 iommu_top 一个小线程回收响应并递减计数。
  - **IOFENCE.C**：扩展 `do_iofence_c` 的等待条件——除 ATS 失效外，还需 `pending_inval_count_ == 0` 才算完成（复用 `iofence_wait_pending_inv` 机制），保证"失效指令 + iofence"语义。
- **CacheMessage 字段扩展**（`iommu/cache_src/common/types.h`）：invalidate 控制区新增 `bool nl`、`bool s_range`（S 位暂只解析不实现范围失效，保留字段）；`InvalidCmdType`/`CacheInvalidateMode` 复用现有枚举，新增 `CacheInvalidateMode::SCAN_RANGE`（小范围枚举扫表）与 `LAZY`（记录 LIB）。

## 二、失效调度优先级改造（cache_subsystem.cpp）

统一原则：**失效任务优先级最高**，各 Cache 的调度进程每轮先查失效 FIFO；命中失效任务时立即处理，否则按原策略（乒乓/优先级）处理常规任务。原有独立的 `*_invalidate_worker_thread` 与调度进程并发访问同一 cache 阵列的竞态问题一并消除：

- **DC Cache**：合并 `dc_worker_thread`/`dc_update_worker_thread`/`dc_invalidate_worker_thread` 为单一 `dc_scheduler_thread`，优先级 invalidate > request/update（request/update 保持先到先服务或乒乓）。响应仍写各自 response fifo。PC、MSIPT 同样处理（PC 必须改，MSIPT 顺带统一）。
- **PT Cache**：`pt_hash_thread` 入口新增 `pt_invalidate_fifo` 检查（最高优先级，在乒乓仲裁之前）：
  - **LAZY（记录 LIB）**：hash 线程内联执行（1~2 拍延时），写 LIB 后立即回响应；
  - **PRECISE / SCAN_RANGE**：hash 线程计算目标 set（精准 1 个 / 枚举 8 个），按 ram_id 生成子失效消息广播到对应 `pt_ram_fifo_`（复用 RAM worker 串行性，保证与 in-flight lookup/fill 的原子性），join 收齐后回 `pt_invalidate_response_fifo`；
  - **SCAN / GLOBAL**：向全部 RAM 广播"扫本 RAM set 区间"子任务，各 RAM worker 扫自己独占的 set 区间并按 `invalidation_compare_per_way_cycles` 计延时，join 聚合 affected_entries。
  - 删除独立 `pt_invalidate_worker_thread`（其逻辑并入上述路径）。
- **Walker Cache**：`walker_hash_thread` 的轮询顺序调整为 **invalidate 最高优先**（当前排第 4 位 → 第 1 位）；失效执行方式与 PT 对齐：LAZY 内联、PRECISE/SCAN_RANGE 按级+ram_id 分发到 `walker_ram_upd_fifo_`（高优先通道）、SCAN/GLOBAL 广播各 worker 扫描本 RAM 区间。
- **响应/join 结构**：为 PT/Walker 各增一个失效 join 计数（复用 walker_join_pending_ 的模式，key=task_id），确保一条失效指令的全部子操作完成后才回响应（IOFENCE 语义依赖此）。

## 三、DC Cache 失效（IODIR.INVAL_DDT）

`execute_dc_invalidate_request` 重写为符合方案的行为：

- **DV=0**：DC `invalidate_global()`；同时向 PC 发 GLOBAL 失效（PC 全清），DID 忽略。
- **DV=1**：DC 按 device_id 精准失效叶级 DDT 条目（现有 `invalidate_ddt`）；同时向 PC 发 SCAN 失效（扫 PC 表，DID 匹配即失效，不做延迟失效）。
- **级联变更**：`enqueue_cascade_invalidations` 从 DC 路径中移除 PT/Walker/MSIPT 级联（spec 中 IODIR 只失效 DDT/PDT 目录缓存），改为 DC→PC 关联失效；`execute_invalidation_pipeline` 的 IODIR_INVAL_DDT 分支同步改为等待 PC 关联失效响应（等待计数逻辑相应简化）。

## 四、PC Cache 失效（IODIR.INVAL_PDT）

- CQ 侧已校验 DV 必须为 1（非法则 cmd_ill），保留。
- PC 按 DID+PID 精准失效（现有 `invalidate_pdt` PRECISE 路径），不再向 PT/Walker 级联（同上按 spec 收敛）。
- PC 替换策略确认为 pLRU（config 已是 plru，不动）。

## 五、PT Cache 失效

### 5.1 新哈希函数（pt_cache.cpp `raw_hash/hash_function`）

替换为方案哈希（输入：16bit gscid、20bit pscid、44bit iova 的页号 PN=iova>>12，无对应项用全 1 占位值）：

```
temp1  = (gscid ^ pscid) & 0b111          // 3bit
temp2  = PN ^ (PN >> 22)
result = ((temp1 << (log2S - 3)) ^ temp2) & (S - 1)   // S = num_sets
```

- ram_id 仍取 `result & (num_rams-1)`，set 拆分逻辑不变。
- 保留旧哈希代码路径，新增 config `pt_cache.hash_mode: "inval_v2"(默认) | "legacy"`，用于回归对比与回退。
- dedup_cache 的哈希不动（去重占位与失效索引无关）。

### 5.2 VMA 失效模式（GV/PSCV/AV 三位编码）

`invalidate_vma` 按方案实现完整模式表：

| GV PSCV AV | 行为 |
|---|---|
| 000 | GLOBAL 清表（同时清 LIB/VN，见第六节） |
| 001 | LAZY：记录 LIB（C=1，仅 PSCID） |
| 100 | LAZY：记录 LIB（C=0，仅 GSCID） |
| 101 | LAZY：记录 LIB（C=2，GSCID+PSCID） |
| 010 / 110 | SCAN_RANGE 小范围枚举扫表（见 5.3） |
| 011 / 111 | PRECISE 哈希精准失效（011 时 Stage2 确定为 bare，按 stage 谓词过滤） |

VMA 仅命中含第一阶段的条目（现有 `stage != STAGE2_ONLY` 谓词保留）；PSCV=1 时全局映射条目（PTE.G=1）除外——cache line 谓词中增加 G 位判断（从缓存的 vs_pte.G 取）。

### 5.3 小范围枚举扫表（addr 有效的 010/110）

新增 `invalidate_by_addr_enum`：
1. `temp2 = PN ^ (PN >> 22)`；
2. 枚举 `temp1 = 0b000..0b111` 共 8 个，计算 `result = ((temp1 << (log2S-3)) ^ temp2) & (S-1)` 得 8 个候选 set；
3. 对每个 set 内逐 way 比较（iova 匹配 + 110 模式再匹配 GSCID），命中置 V=0；延时 = 8 × ways × `invalidation_compare_per_way_cycles`。
4. 调度上作为 8 个子失效分发到对应 RAM worker（见第二节）。

### 5.4 GVMA 失效模式

- GV=0：GLOBAL 清表（清 LIB/VN）；
- GV=1, AV=0：LAZY 记录 LIB（仅 GSCID）；
- GV=1, AV=1：**转换为按 GSCID 扫表**（模式 10 语义，两阶段场景 GPA 无法精准索引）→ 同样走 LAZY 记录 LIB。
- GVMA 仅命中含第二阶段的条目（`stage != STAGE1_ONLY` 谓词）。

### 5.5 NL（非叶失效）语义

- VMA：AV=0 时忽略 NL；AV=1 且 NL=0 → 仅 PT Cache 精准/小范围失效；AV=1 且 NL=1 → PT Cache **和** Walker Cache（三级子表全级别）按 GSCID/PSCID/addr 组合失效（全局映射条目按 PSCV 规则处理）。
- GVMA：GV&AV&NL 均为 1 → 对应 GSCID+ADDR 的所有级别失效（PT + Walker）；其余情况 NL 忽略。
- `execute_invalidation_pipeline` 的 VMA/GVMA 分支据 NL 决定是否向 Walker 发失效。

## 六、基于 VN 的延迟失效（LIB）

新增 `iommu/cache_src/common/lazy_invalid_buffer.h`（纯数据结构，无 SC 进程）：

- **LIB Entry**：`{V:1, C:2(0=仅GSCID/1=仅PSCID/2=两者), TAG1:20(pscid 或占位全0), TAG2:16(gscid 或占位全0), VN:4}`；条目数 `lib_size` 可配（默认 16）。
- **全局 VN**：`CacheSubsystem` 持有 `global_vn_`（位宽 `vn_bits`，默认 4，VN_MAX=15），PT 与 Walker **共享同一 LIB 与 VN**（同一 PTE 失效指令二者需同步响应）。
- **Cache line 增加 VN 字段**：在 `cache_line.h` 的通用 `CacheLine` 增加 `uint8_t vn`（PT 与 Walker 子表复用；DC/PC 不使用不受影响）；`fill/insert` 时记录当前 `global_vn_`。
- **记录流程**（LAZY 命令，hash 线程内联，1~2 拍）：
  1. `global_vn_++`；
  2. LIB 中按 (C,TAG1,TAG2) 匹配：命中则更新该 entry 的 VN，否则写入新 entry（LIB 满时降级为立即扫表失效，保证功能正确）；
  3. 若 `global_vn_ == VN_MAX`：置 `global_vn_=0`，触发**批量扫表**：对 PT Cache 与 Walker 全部 cache line，GSCID/PSCID 命中 LIB 任一 entry 且 `CL.VN < LIB.VN` → invalid；命中但 VN 不小于 → `CL.VN=0` 保留；未命中 → `CL.VN=0`。扫完清空 LIB。批量扫表作为 SCAN 子任务广播到各 RAM worker 执行（占用 RAM 原子段，正确建模扫表延时）。
- **查询路径旁路比对**（`lookup_pt_ram` / `lookup_level_ram` 命中后）：
  - LIB 为空（`lib_count_==0`）→ 直接返回（基线零开销，仅一次整数判断）；
  - 否则并行查 LIB（建模 1~2 拍 CAM，不额外加延时——与 RAM 读并行）：CL 的 gscid/pscid 命中 LIB entry 且 `CL.VN < LIB.VN` → 该 CL 为失效数据：置 V=0、按 miss 返回（走 walk 流程）；否则正常命中。
- **全清联动**：任何 GLOBAL 清表（VMA 000 / GVMA 0* / GLOBAL_INVAL）同步清空 LIB 并复位 `global_vn_=0`。
- **dedup/预取协同**：LIB/VN 只作用于 PT/Walker cache 阵列；dedup 占位与 Buffer 链不受失效影响（in-flight PTW 结果照常刷链，刷入的新 CL 记录当前 VN，天然满足"失效后写入的数据保留"语义）。

## 七、Walker Cache 失效

- 三级子表（C1/C2/C3）与 S2 阵列均支持：GLOBAL / SCAN（含 LIB 批量扫）/ SCAN_RANGE / PRECISE。
- **小范围枚举**：每级子表按自身 `hash_function` 结构实现同样的 temp1 枚举（8 个候选 set），枚举出的 temp1 与该级 addr 段异或得最终 set 索引；实现于 `WalkerSubCache::invalidate_by_addr_enum(level,...)`。
- LAZY/VN 路径与 PT 共享 LIB（第六节），`lookup_level_ram` 命中后做同样旁路比对。
- GVMA GV=1 时 S2 阵列按 GSCID 失效（现有 `invalidate_s2_by_gscid` 复用/接入 LAZY）。

## 八、配置与基线隔离

- `default_config.json` + `json_config.cpp` 新增 `invalidation` 段：
  ```json
  "invalidation": {
    "enable": true,
    "lazy_enable": true,
    "lib_size": 16,
    "vn_bits": 4,
    "lib_match_cycles": 2
  },
  "pt_cache": { "hash_mode": "inval_v2", ... }
  ```
  `GlobalConfig` 增加 `InvalidationConfig` 结构体；`lazy_enable=false` 时扫表类命令走立即 SCAN（回退路径）。
- 场景 5/6/7/8 的 Makefile 目标与测试线程**零改动**：不注入任何失效命令，新逻辑热路径开销仅为 lookup 时一次 `lib_count_==0` 判断。
- 新增专用测试目标 `make TEST=cache_inval`：新测试线程 `rp/test_rp_cache_inval_thread.cc`，复用 `test_rp.hh` 已有的 `iodir/iotinval/iofence` 命令注入接口，覆盖：INVAL_DDT(DV=0/1)→PC 关联失效、INVAL_PDT、VMA 8 模式、GVMA 3 模式、NL=1 联动 Walker、LIB 记录/查询旁路失效/VN 回绕批量扫表、失效后重新翻译触发 PTW 重填。

## 九、实施顺序

1. types.h/CacheMessage/InvalidationCmd 字段与 config 扩展 → 编译通过；
2. PT 新哈希（含 legacy 开关）→ 立即跑场景 5 回归确认命中率/IOPS 不劣化；
3. 调度优先级改造（DC/PC 合并调度、pt_hash/walker_hash 失效最高优先 + RAM 广播/join）；
4. DC→PC 关联失效 + pipeline 分支调整；
5. VMA/GVMA 全模式 + 小范围枚举 + NL 语义；
6. LIB/VN 延迟失效 + 查询旁路 + 批量扫表；
7. CQ 桥接（hook + cq_process_thread + IOFENCE 等待）；
8. 新测试场景 + 全量回归。

## 十、测试计划

- **功能**：`TEST=cache_inval` 全用例通过（每类命令后用 iofence 同步，再次访问验证 miss/重填正确；affected_entries 与日志核对）。
- **单元自检**（可放测试线程内断言）：8 个枚举 result 覆盖实际哈希 set；LIB 命中/未命中/VN 大小比较三分支；VN 回绕后 CL.VN 复位与失效正确性。
- **基线回归**（WSL 编译运行）：场景 5/6/7/8 各跑一遍，比对稳态 IOPS、PT/Walker 命中率、PTW DDR 读数与既有基线一致（波动容差按既有回归规范）；重点确认新哈希与 lookup 旁路判断未引入退化。
- 回归命令沿用现有 `make TEST=seq128k_twostage_s2on` 等目标与 `compile_wsl.sh/run_wsl.sh` 流程。

## Assumptions

- 「精准失效」的 IOVA 按 4KB 页对齐比较（与现有 PT tag 一致）；S 位（NAPOT 范围失效）本期仅解析不实现，非法/未支持组合按 spec 置 cmd_ill 或忽略。
- MSIPT Cache 不在本期方案范围内，仅保持现有 GLOBAL/by-device 行为并接入统一调度风格。
- 批量扫表与 LIB 满降级扫表允许短暂阻塞对应 RAM worker（真实硬件亦如此），该场景不出现在基线中。
- 功能级 `iommu->tlb/ddt_cache/pdt_cache` 的失效逻辑保留（功能正确性由其兜底），性能模型失效通路只影响性能级 Cache 阵列。