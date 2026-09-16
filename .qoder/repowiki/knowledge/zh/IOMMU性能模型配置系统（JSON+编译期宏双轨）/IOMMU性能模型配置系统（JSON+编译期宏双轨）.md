---
kind: configuration_system
name: IOMMU性能模型配置系统（JSON+编译期宏双轨）
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/common/types.h
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - Makefile
---

该仓库的RISC-V IOMMU SystemC性能模型采用**双轨配置机制**：运行时通过轻量级JSON文件加载缓存/失效/统计等参数，编译期通过Makefile `-DTEST_CFG_*`宏定义切换测试场景与功能开关。两者互补，前者用于调参与回归，后者用于快速构建不同测试矩阵。

## 1. 运行时JSON配置系统
- **解析器**：自实现 `mini_json.h`，无外部依赖，支持对象、数组、字符串、布尔、数字及`//`和`/* */`注释。
- **加载入口**：`iommu/cache_src/common/json_config.{h,cpp}` 提供 `load_config(path)` 与 `parse_config(json_str)` 两个API，返回 `GlobalConfig`。
- **配置结构体**：集中在 `iommu/cache_src/common/types.h` 中，包括 `CacheConfig`（sets/ways/replacement/timing）、`InvalidationConfig`（LIB/VN延迟失效）、`StatsConfig`（日志/直方图）以及顶层 `GlobalConfig`。
- **默认值策略**：每个cache段在解析时先填充一组hardcoded默认值（如dc/pc/msipt为64×4 plru，pt为1024×8 srrip），JSON仅覆盖显式字段，缺失字段保持默认。
- **配置文件**：
  - `iommu/cache_config/default_config.json`：完整默认配置，含global、cache_timing、各cache段、walker_cache三级（ptw_c1/c2/c3）、invalidation、statistics。
  - `iommu/cache_config/input_params_example.json`：精简示例，仅包含必要字段，适合快速覆盖。
- **支持的配置项**：
  - `global.clock_period_ns`、`random_seed`
  - `cache_timing.*`：仲裁器、hash、read_set、compare、update_way_select、fill_compute_index（hit/invalid/replacement）、write_way、invalidation_compare_per_way等拍数
  - 各cache段：`num_sets`、`num_ways`、`replacement`（plru/srrip/none）、`srrip_m_bits`、`num_rams`、`ram_fifo_depth`、`hash_mode`（inval_v2/legacy）
  - `walker_cache.base_sets` + ptw_c1/c2/c3独立几何
  - `invalidation.enable/lazy_enable/lib_size/vn_bits/lib_match_cycles`
  - `statistics.output_file/unified_log_file/enable_latency_histogram/enable_task_trace/task_trace_level/histogram_bin_width_ns`

## 2. 编译期宏配置（Makefile TEST场景）
- **场景选择**：通过 `make TEST=<name>` 切换测试线程源文件与编译宏，支持 rand4k_singlestage、seq128k_twostage、rand4k_twostage、sv48_bare、seq512b_2mb_twostage_s2on、cache_inval 等。
- **核心宏命名规范**：`TEST_CFG_<PARAM>=<value>`，由Makefile统一注入，常见包括：
  - `TEST_CFG_PT_DEDUP_PREFETCH_DEPTH`：PT Cache去重预取深度D
  - `TEST_CFG_PTW_WALKER_CACHE_ENABLED`：Walker Cache开关
  - `TEST_CFG_WALKER_CACHE_S2_ENABLED`：S2 Walker Cache开关
  - `TEST_CFG_AXI_PORT_WIDTH_BIT`：AXI端口位宽（512/1024）
  - `TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING`：全局并发上限
  - `TEST_CFG_PTW_MAX_OUTSTANDING_TASKS`：PTW任务并发数
  - `TEST_CFG_SKIP_PHASE1`：跳过第一阶段翻译
- **调试宏**：`DEBUG=1`时自动开启 `DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_MSITRANS`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS` 等。

## 3. 架构与约定
- **分层清晰**：`types.h`定义所有数据结构与默认值 → `json_config.cpp`按段解析并合并默认值 → 上层模块读取 `GlobalConfig` 实例。
- **向后兼容**：JSON字段使用 `contains()` 检查存在性再赋值，缺失字段不报错，保证旧配置仍可运行。
- **多RAM扩展点**：`CacheConfig.num_rams` 与 `ram_fifo_depth` 为未来多RAM并行化预留接口，当前默认单RAM行为。
- **哈希模式可回退**：`hash_mode="inval_v2"` 为新失效哈希，`"legacy"` 用于回归对比。
- **统计输出统一**：`statistics.output_file` 与 `unified_log_file` 二选一，后者作为别名兼容旧路径。

## 4. 约束与规则
- JSON键名必须与 `json_config.cpp` 中 `contains()` 检查的字符串完全一致，否则被静默忽略。
- `replacement` 字段仅接受 `plru`、`srrip`、`none` 三种值，其他值不会触发错误但可能产生未定义行为。
- `num_rams` 必须为2的幂且整除 `num_sets`（由上层校验，非解析器强制）。
- 编译期宏 `TEST_CFG_*` 必须在代码中显式 `#ifdef` 使用，未使用的宏会被编译器忽略。
- 仿真时间固定为 `sc_start(300000000, SC_NS)`（300ms），不在配置文件中暴露。

## 5. 关键文件
- `iommu/cache_src/common/json_config.h/.cpp` — JSON解析与加载API
- `iommu/cache_src/common/mini_json.h` — 轻量JSON解析器
- `iommu/cache_src/common/types.h` — 所有配置结构体定义与默认值
- `iommu/cache_config/default_config.json` — 完整默认配置
- `iommu/cache_config/input_params_example.json` — 精简示例配置
- `Makefile` — 编译期宏与测试场景定义
- `main.cpp` — 仿真入口（不含配置加载逻辑）