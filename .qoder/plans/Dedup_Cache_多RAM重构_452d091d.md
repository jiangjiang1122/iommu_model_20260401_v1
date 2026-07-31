# 去重Cache多RAM重构方案（按用户架构定义）

## 一、目标架构与数据流

```
dedup_request_fifo(16) ────────┐
dedup_inside_request_fifo(32) ─┼─> dedup_scheduler_thread ──> dedup_hash_in_fifo(1) ──> dedup_hash_process_thread
dedup_update_fifo(16) ─────────┘      (轮询仲裁+hash忙闲判断)                                  │ (1cyc hash)
                                                                          ram_id = org_hash & (N-1)
                                                                                   │
                                              ┌──────────────┬──────────────┬─────┴────────┬──────────────┐
                                        RAM_FIFO_0(8)   RAM_FIFO_1(8)   RAM_FIFO_2(8)   RAM_FIFO_3(8)
                                              │              │              │              │
                                     dedup_ram_worker(0) worker(1)      worker(2)      worker(3)
                                       原子段5cyc: get_set(2)+get_free_line(2)+write(1)
                                       分支1/2/3处理 + Buffer交互 + 响应路由
                                              │
                              ├─ HIT(挂Buffer) ──> pt_hit_response_fifo (dedup_suspended=1)
                              ├─ MISS ──────────> pt_miss_response_fifo ──> PTW
                              ├─ MISS+预取使能 ──> 生成D个预取任务写 dedup_inside_request_fifo
                              └─ UPDATE ────────> flush回调 + clear line
```

核心变化：
- 单调度线程串行 → 1 Scheduler + 1 Hash + N RAM Worker（N默认4，可配置）
- 预取占位从「MISS内联循环insert」→「独立预取任务经 dedup_inside_request_fifo 重新调度」，hash到不同RAM后并行执行
- Hash单元空闲即可读下一任务（不等前一任务整体完成），同RAM原子段串行、跨RAM并发

## 二、关键决策

1. **仲裁策略**（dedup_scheduler_thread）：保持 REQUEST/UPDATE 乒乓不变；REQUEST 类内 `dedup_inside_request_fifo` 优先于 `dedup_request_fifo`（预取任务尽早占位，避免后续任务MISS重复走PTW）。
2. **Hash忙闲建模**：Scheduler 与 Hash 单元之间用深度1的 `dedup_hash_in_fifo` 连接——写满即代表 Hash 忙，Scheduler 阻塞写实现天然反压（反压传导至三个入口FIFO）。
3. **RAM散列拆分**：`org_hash & (num_rams-1)` 为RAM号（低位选RAM，保证连续iova页分散到不同RAM）；`org_hash >> log2(num_rams)` 为RAM内set索引。`global_set = ram_id * sets_per_ram + set_in_ram`，每RAM独占连续set区间，跨RAM零竞争。
4. **Buffer交互留在RAM Worker内**（按用户进程定义）：Buffer满时 `wait(free_event)` 阻塞该RAM Worker（仅阻塞1组RAM，其余3组继续工作；该RAM前置FIFO满后反压Hash单元）。
5. **预取任务标记**：`CacheMessage` 新增 `bool dedup_is_prefetch`；预取任务在RAM Worker中执行"先lookup（已存在则跳过）→ insert(is_req=0, head_index=0xFFFF)"，不产生对外响应、不碰Buffer。
6. **UPDATE路径同样经Hash分发**：UPDATE需查dedup_cache line，必须路由到对应RAM；RAM Worker执行 lookup + (主占位时)flush回调 + clear_line，flush回调阻塞时间计入该RAM忙时。
7. **配置参数化**：JSON新增 `dedup_cache` 段（`num_rams` 默认4、`ram_fifo_depth` 默认8），不再复用pt_cache配置；num_rams须为2的幂且整除num_sets。

## 三、进程与FIFO定义

### 进程（1+1+N=6个，N=4）
| 进程 | 注册方式 | 职责 |
|------|---------|------|
| `dedup_scheduler_thread` | SC_THREAD | 轮询3入口FIFO（乒乓+inside优先），hash空闲则下发 |
| `dedup_hash_process_thread` | SC_THREAD | 1cyc hash → 按ram_id写RAM_FIFO（满则阻塞保持忙） |
| `dedup_ram_worker_thread(0..3)` | sc_spawn | 原子段5cyc + 分支处理 + Buffer交互 + 响应路由 |

### FIFO
| FIFO | 深度 | 写入方 | 读取方 |
|------|------|--------|--------|
| `dedup_request_fifo` | 16（现有） | PT Cache RAM Worker (MISS) | scheduler |
| `dedup_update_fifo` | 16（现有） | PTW Monitor | scheduler |
| `dedup_inside_request_fifo` | **32**（新增） | dedup RAM Worker (MISS预取) | scheduler |
| `dedup_hash_in_fifo` | **1**（新增） | scheduler | hash线程 |
| `dedup_ram_fifo_[0..3]` | **8**（新增，可配置） | hash线程 | 对应RAM Worker |

失效FIFO本期不建模（按用户要求）。

## 四、RAM Worker分支逻辑（从execute_dedup_request迁移）

REQUEST（普通任务，dedup_is_prefetch=0）：
- 分支1 HIT主占位(is_req=1)：读链头tail_index → allocate_entry(满则wait free_event) → 挂链尾 → 更新链头tail_index（仅写Buffer）→ `dedup_suspended=1` 推 `pt_hit_response_fifo`
- 分支2 HIT预取占位(is_req=0)：allocate_entry → 填entry(tail自引用) → 升级line(head_index=new, is_req=1) → `dedup_suspended=1` 推 `pt_hit_response_fifo`
- 分支3 MISS：allocate_entry → insert主占位(is_req=1)
  - 成功：若 prefetch_enabled && D>0，按页边界裁剪D后生成D个预取任务（`dedup_is_prefetch=1`，iova=主iova+d*4KB）写 `dedup_inside_request_fifo`；主任务推 `pt_miss_response_fifo` → PTW
  - 失败（set全为is_req=1受保护）：free_entry、`dedup_bypass=1`、禁预取，推 `pt_miss_response_fifo`

REQUEST（预取任务，dedup_is_prefetch=1）：
- lookup已存在 → 跳过（消耗原子段后结束）；否则 insert(is_req=0, head_index=0xFFFF)。无响应输出。

UPDATE：
- lookup MISS → 无操作；HIT预取占位 → clear_line；HIT主占位 → `dedup_flush_cb_(head_index, upd)` 刷Buffer链后 clear_line。

每类操作结束统一 `wait(DEDUP_ATOMIC_CYCLES=5cyc)` 消耗原子段（get_set 2 + get_free_line 2 + write 1），同RAM串行由单worker天然保证。

## 五、文件变更清单

1. **dedup_cache.h/cpp**：新增 `configure_multi_ram(num_rams)` / `raw_hash()` / `compute_ram_id()`；`hash_function()` 改为多RAM映射（num_rams=1退化为旧行为）；lookup/insert/clear_line保持纯功能接口不变（供RAM Worker调用）。
2. **types.h**：`GlobalConfig` 新增 `CacheConfig dedup_cache`（复用num_rams/ram_fifo_depth字段）；`CacheMessage` 新增 `bool dedup_is_prefetch`。
3. **json_config.cpp / default_config.json**：解析 `dedup_cache.num_rams=4, ram_fifo_depth=8, num_sets/num_ways`（默认与pt_cache同几何）。
4. **cache_subsystem.h**：新增3类线程声明、`dedup_inside_request_fifo`/`dedup_hash_in_fifo`/`dedup_ram_fifo_`、DEDUP_MAX_RAMS=16、Hash单元与每RAM统计字段（任务数/忙时/反压/FIFO峰值，参照PT Cache命名）、`print_dedup_multi_ram_report()`、`dedup_ram_fifo_pending()`。
5. **cache_subsystem.cpp**：
   - 构造函数：创建FIFO与sc_spawn RAM Worker，SC_THREAD注册scheduler/hash线程
   - `dedup_scheduler_thread` 重构为纯调度（读3 FIFO → 阻塞写 hash_in_fifo）
   - 新增 `dedup_hash_process_thread`（参照 `pt_hash_thread`：1cyc + compute_ram_id + 阻塞写RAM FIFO + 反压统计）
   - 新增 `dedup_ram_worker_thread(int)`（携带第四节分支逻辑；原 `execute_dedup_request/execute_dedup_update` 逻辑迁入，函数保留改为worker内调用或直接内联后删除）
   - REQUEST/UPDATE统计（dedup_req_count_等）迁移到RAM Worker，按ram_id分组新增
6. **iommu_perf_params.hh**：保留 `DEDUP_ATOMIC_CYCLES=5`；`DEDUP_HASH_CYCLES=1` 由hash线程显式消耗（不再是"隐藏重叠"注释语义）。
7. **iommu_top.cc**：终局drain增加 `dedup_ram_fifo_pending()==0 && dedup_hash_in_fifo空 && dedup_inside_request_fifo空` 判断；统计输出处调用 `print_dedup_multi_ram_report()`。

## 六、时序流水验证要点（对照用户示例）

- 任务1(iova=0x0) MISS：hash 1cyc → RAM0原子段5cyc → 3个预取任务入inside_fifo，经hash分散到不同RAM并行执行
- 任务2~8(同4KB页)：hash到RAM0同FIFO，原子段串行（每5cyc一个），分支1 HIT主占位
- 任务9(iova=0x1000)：hash到RAM1，与RAM0任务完全并发
- 稳态：4组RAM并发，查询出口理想吞吐≈1结果/cycle

## 七、死锁风险与规避

1. RAM Worker写 `dedup_inside_request_fifo` 满 → 阻塞该Worker → 若scheduler又因hash反压无法消费inside_fifo → 环路。规避：inside_fifo深度32（≥ 最大并发MISS×D），且scheduler中inside优先消费；预取写入使用 `nb_write`，满则丢弃该预取任务（预取是优化非功能必需，丢弃仅损失性能）并计数统计。
2. Buffer满阻塞RAM Worker：与现状语义一致（原来阻塞整个scheduler，现在仅阻塞1组RAM），风险不升级。

## 八、验证计划

1. WSL编译 `make TEST=seq128k_twostage_s2on -j4` 零错误
2. 场景5功能回归：10000/10000 PASS，PA结果正确
3. 性能回归：稳态IOPS ≥ 122.64M（重构前基线）
4. Multi-RAM报告检查：4组RAM任务分布均匀、Hash反压统计、RAM FIFO峰值、预取任务丢弃计数=0（正常负载下）
5. 日志核对：`[DEDUP_*]` 分支日志与重构前语义一致（去重链长、flush任务数不变）

## 假设

1. 仲裁细节：inside_fifo 优先于 request_fifo（用户带问号的倾向，采纳；后续可调）
2. dedup_cache几何默认与pt_cache相同（1024 sets × 8 ways），独立JSON段可单独调整
3. UPDATE经hash分发到RAM Worker执行（用户图中update_fifo经过Hash，采纳）
4. 预取任务inside_fifo满时降级为丢弃（保功能防死锁）
