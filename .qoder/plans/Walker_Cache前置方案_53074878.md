# Walker Cache 前置 + 多RAM(4组) 方案

## Summary

两项核心改动：
1. **查询前置**：所有输入请求在 `configure_and_route`（PT Cache 查询发起点）同时发起 Walker Cache 查询；结果记录到任务信息（`walk_ctx` 新增字段）并一路透传（去重模块不感知），PTW 直接使用任务携带的结果，不再自行查询（严格前置，已确认）。
2. **多RAM重构**：C1/C2/C3 三级子表各自按本级 set 哈希低 2 位分成 4 组 RAM；一次查询拆成最多 3 个子查询并行进入不同 RAM Worker，join 后按 C3>C2>C1 仲裁（已确认三级子表独立分RAM方案）。

基线保护：新增编译开关 `TEST_CFG_WALKER_FRONT_ENABLED`（默认 1），置 0 时恢复 PTW 内查询的旧路径；`num_rams=1` 时多RAM退化为单 Worker。S2 Cache 查询（GS_EXPLICIT 阶段，key 为中途才产生的 GPA）**无法前置，保留在 PTW 中**，走独立 FIFO 通道与前置查询隔离。

## 关键设计

### A. 前置查询接线（iommu_top 层）
- `configure_and_route`（iommu_top.cc L448）：写 `pt_request_fifo` 的同时，将任务注册进新增 `walker_front_pending` map（mutex 保护），并写新增 `walker_front_request_fifo`（深度16）。
- 新增 SC_THREAD `walker_front_response_thread`：读 `walker_front_response_fifo`，按 task_id 查 pending map：
  - 命中 map：把结果写入 `task->walk_ctx` 新增字段 `walker_front_valid / walker_front_hit / walker_front_level / walker_front_next_ppn`，置 `walker_result_ready=true`，notify 全局事件 `walker_front_ready_event`；
  - map 中已移除（PT HIT 已直接输出）：丢弃响应，不触碰 task（防 use-after-free）。
- `collector_pt_response_thread` HIT 常规 CL 分支：转发 Forwarder 前先从 `walker_front_pending` 移除该 task_id。
- PTW 输入侧（`ptw_req_process_thread` 读 `pt_cache_to_ptw_fifo` 后）：若 `!walker_result_ready` 则 `wait(walker_front_ready_event)` 循环等待（正常时序下 Walker 3~4 拍 << dedup 路径 7+ 拍，等待为罕见兜底）。

### B. PTW 改动（iommu_perf_ptw.cc L224-L340）
- `TEST_CFG_WALKER_FRONT_ENABLED=1` 时：删除 VS-stage 的 walker_request/response FIFO 往返与 `walker_cache_mtx` 加锁，改为读取任务携带的 `walker_front_*` 字段，复用现有 `walker_response_to_task` 的解释逻辑（拆出为 `apply_walker_front_result(task)`：设置 `walk_ctx.level = 3 - hit_level`、`base_addr`、`vs_l0_spa_ppn`(hit_level=3 时)、`walker_hit_level`）。
- 后续逻辑（两阶段跳 GS_IMPLICIT、L0 命中合并 burst 预取、完成后 update Walker Cache 判定 kind）全部不变。
- GS_EXPLICIT 的 S2 lookup 保持现状（仍走 `walker_request_fifo`/`walker_response_fifo` + mutex），该通道今后仅承载 S2 查询，与前置通道物理隔离，天然消除响应错配。

### C. Walker Cache 多RAM（三级子表独立分RAM）
- `WalkerSubCache`（walker_cache.h/.cpp）参照 `PTCache` 增加：`raw_hash()`（现有 va_seg^gscid^pscid 去掩码）、`compute_ram_id() = raw & (num_rams-1)`、set 重映射 `set = ram_id*sets_per_ram + ((raw>>log2) & (sets_per_ram-1))`、原子段接口 `lookup_ram/update_ram/lookup_s2_ram/update_s2_ram`（返回 ram_latency，不含 wait）。
- `cache_subsystem` 以「Hash线程 + 4 RAM Worker + Join」替换 `walker_scheduler_thread`：
  - `walker_hash_thread`：轮询 `walker_front_request_fifo`（前置lookup）→ `walker_request_fifo`（S2 lookup）→ `walker_update_fifo` → `walker_invalidate_fifo`；hash 1 拍后将消息按级拆分为子操作（lookup 拆 C3/C2/C1 三条，Sv39 时无 C1；update 按 `walker_update_kind` 拆 1~3 条；S2 同理），各自按本级 `compute_ram_id` 写入 `walker_ram_fifo_[id]`（深度 `ram_fifo_depth`，满则阻塞反压）。
  - `walker_ram_worker_thread(i)`：串行消耗本 RAM 子操作原子段延时；lookup 子响应写 `walker_join_fifo`；update/invalidate 子操作完成后向 join 报数。
  - `walker_join_thread`：按 (origin, task_id) 聚合子响应，收齐后仲裁 C3>C2>C1，最终响应按 origin 写 `walker_front_response_fifo` 或 `walker_response_fifo`；invalidate 聚合 affected_entries 后回 `walker_invalidate_response_fifo`。
  - 同级两个子查询落在同一 RAM 时自然串行（与硬件语义一致）；`num_rams=1` 时全部子操作进同一 Worker，时序等价旧串行模型。
- `CacheMessage`（types.h）新增少量字段：`walker_origin`（FRONT/PTW_S2/INVAL）、`walker_sub_level`（子操作目标级）、`walker_sub_expected`（join 期望数）。

### D. 配置与开关
- `default_config.json`：`walker_ptw_c1/c2/c3` 三节各加 `"num_rams": 4, "ram_fifo_depth": 8`（`CacheConfig` 已有这两个字段，确认/补齐 json_config.cpp 对 walker 三节的 num_rams 解析）。
- `iommu_perf_params.hh`：新增 `TEST_CFG_WALKER_FRONT_ENABLED`（默认 1）→ `WALKER_FRONT_ENABLED` 常量；置 0 时 configure_and_route 不发前置查询、PTW 走旧查询路径（旧路径代码保留在 `#if`/运行时分支中）。
- `iommu_task.hh`：`walk_ctx` 新增 `walker_front_valid/hit/level/next_ppn、walker_result_ready` 字段。

### E. 统计口径
- Walker Cache lookup 统计语义变化：由"仅 PTW 任务"（~1.6k 次）变为"全部输入请求"（10000 次），命中率数值会变，属预期；新增前置查询计数、per-RAM 任务数/忙时/FIFO 峰值统计（照抄 PT Cache 的 `pt_ram_*` 系列）与 Hash 反压计数。
- PTW 的 `[PTW_STAT] Walker_HIT` 打印与 DDR 分布统计逻辑不变（数据源改为任务携带结果）。

## 修改文件清单
1. `iommu/include/iommu_task.hh` — walk_ctx 新增前置结果字段
2. `iommu/cache_src/common/types.h` — CacheMessage 新增 walker_origin/sub_level 字段
3. `iommu/cache_src/cache/walker_cache.h/.cpp` — SubCache 多RAM哈希重映射 + 原子段接口
4. `iommu/cache_src/subsystem/cache_subsystem.h/.cpp` — walker_front FIFO 对、hash/worker/join 线程、per-RAM 统计
5. `iommu/iommu_top.hh/.cc` — 前置查询发起、pending map、front_response 线程、HIT 路径清理
6. `iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc` — HIT 分支移除 pending 项
7. `iommu/iommu_perf_model/iommu_perf_ptw.cc` — VS-stage 查询改用任务携带结果（含开关分支）+ PTW 输入 ready 兜底等待
8. `iommu/iommu_perf_model/iommu_task_cache_convert.cc` — 拆出 apply_walker_front_result
9. `iommu/iommu_perf_model/iommu_perf_params.hh` — WALKER_FRONT_ENABLED 开关
10. `iommu/cache_config/default_config.json`（及 json_config.cpp 解析确认）— walker 三节 num_rams=4

## Test Plan
1. **编译**：WSL 下三个场景目标均零错误。
2. **功能冒烟**：场景5 先跑 100 包，确认 0 fault、前置查询计数=请求数、PTW 日志中 Walker_HIT 语义正常、无死锁。
3. **基线回归**（核心验收）：场景5/6/7 各 10000 包，对比基线 `6d4d639`：
   - 场景5 稳态 IOPS ≥ 122.66M；场景6 = 250.00M（线速）；场景7 ≥ 76.29M；均 0 fault
   - 顺序场景重点观察：严格前置下"输入时 MISS、到 PTW 时本可 HIT"的任务是否导致 DDR reads 分布退化（对比 24 次全 MISS 任务占比）
4. **开关验证**：`TEST_CFG_WALKER_FRONT_ENABLED=0` 编译场景5 跑 1000 包，行为与基线一致（旧路径未破坏）。
5. **多RAM退化验证**：JSON 改 `num_rams=1` 跑场景5 1000 包，结果与 4RAM 功能一致。
6. 若场景5/6 稳态 IOPS 下降超过 1%：按既定预案增加"PTW 二次校验"编译开关（本期不实现）。

## Assumptions
- S2 Cache（GPA key）查询保留在 PTW，属需求范围内的合理边界（GPA 在 walk 中途才产生，物理上无法前置）。
- 预取任务复用主任务 walker 上下文的现有行为不变。
- Dedup/Buffer 模块零改动，任务信息经由 task 指针天然透传。
- Walker update 仍为 PTW 完成后 fire-and-forget（无响应等待），与现状一致。