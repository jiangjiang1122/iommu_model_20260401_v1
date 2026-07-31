---
kind: configuration_system
name: JSON配置文件系统（Cache参数与统计）
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
    - iommu/iommu_top.hh
---

该仿真平台使用自实现的轻量级 JSON 解析器加载 IOMMU Cache 子系统的运行时配置，所有缓存几何、替换策略、时序延迟以及统计输出均由 JSON 文件驱动，无需重新编译即可调整模型行为。

**系统与工具**
- 自定义 mini_json：`iommu/cache_src/common/mini_json.h` 提供无第三方依赖的 JSON 解析器，支持对象、数组、布尔、整数、浮点、字符串及 `//` 和 `/* */` 注释。
- 配置加载接口：`iommu/cache_src/common/json_config.{h,cpp}` 暴露 `load_config(path)` 与 `parse_config(json_str)` 两个入口，返回统一的 `GlobalConfig` 结构体。
- 配置数据结构：`iommu/cache_src/common/types.h` 中定义 `CacheConfig`、`StatsConfig`、`GlobalConfig` 等结构体，作为 JSON 字段到 C++ 类型的映射目标。

**关键文件与位置**
- 解析器：`iommu/cache_src/common/mini_json.h`
- 加载逻辑：`iommu/cache_src/common/json_config.h` / `.cpp`
- 类型定义：`iommu/cache_src/common/types.h`（含 `CacheConfig`、`StatsConfig`、`GlobalConfig`）
- 默认配置：`iommu/cache_config/default_config.json`
- 示例配置：`iommu/cache_config/input_params_example.json`
- 顶层集成：`iommu/iommu_top.hh` 中通过 `iommu::load_config("iommu/cache_config/default_config.json")` 在模块构造时加载配置并传入 `CacheSubsystem`。

**架构与约定**
- 分层默认值：每个 cache 段（dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache）先以 `common_cache_def` 为基线，再叠加各自默认几何（如 pt_cache 默认 1024×8、srrip_m_bits=2），最后用 JSON 中显式字段覆盖。
- 可选字段安全读取：所有 JSON 键均通过 `j.contains(key)` 检查后再赋值，缺失字段保持默认值，避免硬错误。
- Walker Cache 三级分层：`walker_base_sets` 统一控制 C1/C2/C3 的 set 数，各层独立设置 ways 与 replacement 策略。
- 统计输出集中化：`statistics` 段统一控制日志文件、延迟直方图开关、task trace 级别与直方图 bin 宽度。
- 配置来源单一：当前仅支持从文件系统 JSON 加载，未实现环境变量或命令行参数覆盖；`main.cpp` 不传参，配置路径硬编码于 `iommu_top.hh`。

**约束与规则**
- JSON 键名必须与 `json_config.cpp` 中的 `contains()` 分支严格一致（如 `num_sets`、`num_ways`、`replacement`、`srrip_m_bits`、`clock_period_ns`、`random_seed` 等）。
- `cache_timing` 下的延迟字段（`arbiter_latency_cycles`、`hash_latency_cycles`、`read_set_latency_cycles` 等）按拍数单位解释，直接影响性能建模。
- `replacement` 字段仅接受 `plru`、`srrip`、`none` 三种策略，由替换算法实现限定。
- `num_rams` 与 `ram_fifo_depth` 用于多 RAM 分组去重缓存，需满足 `num_sets % num_rams == 0` 的隐含约束（代码中未显式校验）。
- 统计输出文件路径由 `statistics.output_file` 或兼容字段 `unified_log_file` 指定，后者在前者缺失时回退。