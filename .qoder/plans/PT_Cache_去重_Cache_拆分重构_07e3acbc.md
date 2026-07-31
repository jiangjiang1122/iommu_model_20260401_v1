# PT Cache / 去重 Cache / Buffer 拆分重构方案

## 目标
把当前内嵌在 `execute_pt_request()`（[cache_subsystem.cpp](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/subsystem/cache_subsystem.cpp)）里的去重+预取逻辑，以及散落在 `PTData.reserved` 里的占位字段（`is_ph/is_req/head_index`），抽取为独立的 **dedup_cache** 模块。PT Cache 变为纯常规缓存；DedupBuffer 结构不变。

关键决策（已确认）：
- 时序：**精确流水线建模**（hash 与前一任务原子段重叠，原子段串行，稳态约 5cyc/任务，MISS+D=3 消耗 1+3 个原子段）。
- 刷新编排：**保留 group Monitor**；Monitor 写 `dedup_update_fifo`，由 `dedup_scheduler_thread` 完成 dedup_cache 查表 + buffer 刷新 + 置 V=0。

---

## 一、PT Cache 变更（纯常规缓存）

文件：[pt_cache.h](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/cache/pt_cache.h) / [pt_cache.cpp](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/cache/pt_cache.cpp) / [types.h](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/common/types.h)

- 删除 PTCache 的占位接口：`insert_placeholder` / `update_placeholder` / `update_placeholder_with_data` / `batch_update_placeholders`，仅保留 `lookup_pt` / `fill_pt` / `invalidate_*`。
- 从 `pt_reserved_t`（types.h L215-231）移除 `is_ph` / `head_index` / `is_req` 三个位域，归还给 `reserved` 保留位。其余字段（valid/trans_type/sv48/gstage_x4 等）保留。
- `execute_pt_request()` 重写为纯逻辑：
  - HIT 常规 CL → 填充 PTE 到 resp，`pt_hit_response_fifo`（行为不变）。
  - MISS → 不再插入占位、不再写 `pt_miss_response_fifo`，改为把请求（携带 `task_ptr`、`prefetch_enabled`、`prefetch_depth`、`timestamp`）转发到新增 `dedup_request_fifo`。
- `execute_pt_update_request()` 不变（`fill_pt` 覆盖为常规 CL）。
- `pt_scheduler_thread` 乒乓调度（REQUEST/UPDATE、`pt_sched_next_is_request_`）保持不变，仅 MISS 分支出口改为投递 `dedup_request_fifo`。

## 二、新增 dedup_cache 模块

新增文件：`iommu/cache_src/cache/dedup_cache.h` + `dedup_cache.cpp`（加入 [Makefile](file:///d:/Qoder_proj/iommu_model_20260401_v1/Makefile) 源列表与 `build_wsl.sh`）。

### Cache line 结构（DedupCacheLine）
字段：`gscid, pscid, iova, head_index, is_req, V`。不设 `is_ph`（`V=1` 即代表占位存在）。几何（num_sets/num_ways）新增 dedup_cache 配置项，默认复用 pt_cache 几何。

### 自定义替换策略（无 LRU，命中不更新替换信息）
`insert()` 按 set 定位后：
1. 有 `V=0` 的 way → 直接写入；
2. 否则有 `V=1 & is_req=0`（预取占位）的 way → 随机淘汰其一并写入；
3. 否则（全部 `is_req=1` 受保护）→ 插入失败，返回 false（调用方降级：释放 Buffer、置任务 `dedup_bypass=true`、直接转发 PTW）。

### CacheSubsystem 集成（[cache_subsystem.h](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/subsystem/cache_subsystem.h) / .cpp）
- 新增成员：`std::unique_ptr<DedupCache> dedup_cache_`；`sc_fifo<CacheMessage> dedup_request_fifo, dedup_update_fifo`；`dedup_buffer_` 归属保持。
- 新增 `SC_THREAD(dedup_scheduler_thread)`，与 `pt_request/update` 相同的乒乓模式处理 `dedup_request_fifo` / `dedup_update_fifo`（`dedup_sched_next_is_request_` 标志；两空 `wait()` 双事件）。
- 新增 `execute_dedup_request()` / `execute_dedup_update()`。
- 新增转发句柄：`set_forward_fifo(sc_fifo<iommu_task_t*>*)`，由 iommu_top 绑定 `pt_cache_to_fwd_fifo`，供 update 刷新 buffer 时转发任务。

### execute_dedup_request（查询流程，含 buffer 交互）
从 `dedup_request_fifo` 取请求，dedup_cache 查表（hash）后按分支处理（复用现有 [cache_subsystem.cpp](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/subsystem/cache_subsystem.cpp) L746-1004 的 buffer 挂接代码，迁移到此）：
- 分支1 HIT 主占位（V=1,is_req=1）：读链头 `tail_index` → `allocate_entry` → 挂 `Buffer[tail].next=new` → 更新链头 `tail_index=new`（仅写 Buffer）。任务挂起 → 推 `pt_hit_response_fifo` 且带 `dedup_suspended=1` 标记。
- 分支2 HIT 预取占位（V=1,is_req=0）：`allocate_entry`（满则 `wait(free_event)`）→ 填 entry（`tail=new`）→ 更新 dedup_cache 该 line `head_index=new, is_req=1`。任务挂起 → 推 `pt_hit_response_fifo` + `dedup_suspended=1`。
- 分支3 MISS：`allocate_entry`（满则阻塞）→ 填 entry（`tail=head`）→ `dedup_cache.insert()` 主占位（is_req=1）。
  - 预取使能且 D>0：页表页边界内（`vpn0_in_page`，最多 511）顺序 `insert()` D 个预取占位（is_req=0, head_index=0xFFFF），已存在则跳过。
  - 推 `pt_miss_response_fifo`（携带 `prefetch_enabled/depth`）→ 经现有 `collector_pt_response_thread` → PTW。
  - 降级（insert 失败）：`free_entry`、`dedup_bypass=1`、`prefetch_enabled=0`，推 `pt_miss_response_fifo` → PTW（不去重不预取）。

### 精确流水线时序（execute_dedup_request 内）
在 `dedup_scheduler_thread` 维护原子段空闲水位 `dedup_atomic_free_time_`。周期常量新增到 `iommu_perf_params.hh`：`DEDUP_HASH_CYCLES=1`、`DEDUP_GET_SET_CYCLES=2`、`DEDUP_GET_FREE_LINE_CYCLES=2`、`DEDUP_WRITE_LINE_CYCLES=1`（原子段=5cyc）。
- 单任务：原子段 = 5cyc；hash(1cyc) 作为流水填充只加到首任务/端到端延时。
- 原子段串行：`atomic_start = max(now, dedup_atomic_free_time_)`；`dedup_atomic_free_time_ = atomic_start + atomic_cycles`；`wait` 至结束。稳态吞吐 ≈ 5cyc/任务。
- MISS+预取：主任务 + D 个预取，各消耗一次原子段（共 (1+D)×5cyc），串行累加到水位。
- 说明（假设）：需求书注1中"get cache set=1cyc"与注2/3的"2cyc"不一致，本方案取注2/3（稳态口径）2cyc，作为可调常量。

## 三、PTW 完成后刷新（保留 group Monitor）

文件：[iommu_perf_ptw.cc](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/iommu_perf_model/iommu_perf_ptw.cc) `prefetch_group_monitor_thread`（L2255+）、D=0 路径（L2150+）。

- Monitor 仍按 group 批量收集。无锁阶段对每个完成 iova：
  1. 写 `pt_update_fifo`（PT Cache fill 常规 CL，保持现有 L2491-2508 逻辑）；
  2. **改为**写 `dedup_update_fifo`（携带该 iova 的已解析 PTE：`pa` 页基址、`vs_pte`、`g_pte`、`page_sz`，及 `main_task` 供日志），**不再**直接调用 `flush_dedup_buffer_by_iova`。
- 对 `dedup_bypass=1` 的任务：只写 `pt_update_fifo`，不写 `dedup_update_fifo`（PTW 完成不刷 buffer）。
- fault 组：维持对 PT Cache 的 `PT_INVALIDATE`；dedup 占位由 `execute_dedup_update` 查表后置 V=0（fault 时按 is_req 处理，不转发或按现有 fault 语义丢弃）。
- D=0 直刷路径（L2150-2173）：同样改为写 `dedup_update_fifo`，移除直接 `flush_dedup_buffer_chain` 调用。

### execute_dedup_update（dedup 模块内，含 buffer 刷新）
从 `dedup_update_fifo` 取更新（乒乓）：
- 任务1 刷 dedup_cache：dedup_cache 查表(iova)；MISS→结束；HIT→取 `head_index/is_req`。
- 若 `is_req=0`：buffer 未记录实际任务，直接置该 line `V=0`，结束。
- 若 `is_req=1`：执行任务2（先刷 buffer，完成后再置 V=0）：
  - 任务2 刷 buffer：从 `head_index` 沿 `next_index` 遍历链（迁移 [iommu_perf_pt_dedup_flush.cc](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/iommu_perf_model/iommu_perf_pt_dedup_flush.cc) 的 `flush_dedup_buffer_*` 逻辑到 CacheSubsystem）：为每挂起任务算 `PA=(PTE.pa & ~0xFFF)|offset`，写回 `vs_pte/g_pte/page_sz`，置 `state=TASK_PTW_DONE`，**先 `free_entry` 再** 经绑定的 `pt_cache_to_fwd_fifo` 转发。
  - 全链处理完后置 dedup_cache 该 line `V=0`。
- 说明：需求书提到"update 会一直锁住 dedup cache 访问"——因 update 与 request 共用单调度线程串行，天然互斥，无需额外锁。

## 四、DedupBuffer 保持不变
[dedup_buffer.h](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/common/dedup_buffer.h) 结构/接口（`allocate_entry/free_entry/free_event/tail_index/next_index`）不变。仅调用方从 PT Cache 路径迁移到 dedup_cache 路径。

## 五、响应路由与集成点
- [iommu_perf_pt_cache_response.cc](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc)：判定"挂起"从 `resp.pt_data.reserved.is_ph==1` 改为新标记 `resp.dedup_suspended`（CacheMessage 新增 bool）；MISS 路径（→ PTW）不变。
- [iommu_top.cc](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/iommu_top.cc)：构造期调用 `cache_sub.set_forward_fifo(&pt_cache_to_fwd_fifo)`；初始化 dedup_cache；`flush_dedup_buffer_*` 从 iommu_top 移除（逻辑已迁入 CacheSubsystem）。
- [types.h](file:///d:/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/common/types.h) `CacheMessage` 新增：`bool dedup_suspended`、`bool dedup_bypass`、dedup_update 的 PTE 载荷（可复用现有 `pt_data`/`pa` 字段）。

## 六、编译与验证
- 更新 [Makefile](file:///d:/Qoder_proj/iommu_model_20260401_v1/Makefile) 与 `build_wsl.sh` 加入 `dedup_cache.cpp`。
- 用现有 20 包/50 包单阶段场景（需显式启用 D=3 预取与 Walker Cache）回归：验证命中/去重/预取行为与拆分前一致，出口任务数与 PA 结果不变。
- 静态检查后按项目规范执行 WSL 编译与小规模仿真核对日志（`[DEDUP_*]` 标记）。

## 假设
1. dedup_cache 几何默认复用 pt_cache 的 num_sets/num_ways（如需独立配置再加 JSON 项）。
2. 原子段"get cache set"取 2cyc（注2/3稳态口径）。
3. 大页仍"暂不实现"（沿用现有约束，代码保留不激活）。
4. dedup buffer 刷新迁入 CacheSubsystem，经绑定的 `pt_cache_to_fwd_fifo` 转发（避免跨模块回调）。