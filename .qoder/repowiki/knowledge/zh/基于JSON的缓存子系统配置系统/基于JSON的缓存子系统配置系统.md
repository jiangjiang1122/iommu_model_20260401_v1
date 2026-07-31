---
kind: configuration_system
name: 基于JSON的缓存子系统配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/cache_src/subsystem/cache_subsystem.cpp
---

本仓库采用轻量级JSON配置文件驱动IOMMU性能模型中Cache子系统的运行时配置，核心由 `iommu/cache_src/common/json_config.{h,cpp}` 提供解析能力，配合 `iommu/cache_config/` 下的默认与示例配置文件使用。

## 1. 使用的系统与工具
- **配置格式**：JSON（自研 `mini_json.h` 解析器，无外部依赖）
- **加载入口**：`load_config(json_path)` / `parse_config(json_str)`
- **配置结构体**：`GlobalConfig`、`CacheConfig`、`StatsConfig` 定义于 `iommu/cache_src/common/types.h`
- **配置消费方**：`CacheSubsystem` 构造函数接收 `GlobalConfig`，据此实例化 DC/PC/MSIPT/PT/Walker 各 Cache 模块并设置统计输出

## 2. 关键文件与包
- `iommu/cache_src/common/json_config.h/.cpp` — JSON 解析与合并默认值逻辑
- `iommu/cache_src/common/types.h` — `GlobalConfig`/`CacheConfig`/`StatsConfig` 等类型定义
- `iommu/cache_config/default_config.json` — 完整默认配置
- `iommu/cache_config/input_params_example.json` — 用户可编辑的示例配置
- `iommu/cache_src/subsystem/cache_subsystem.cpp` — 读取 `GlobalConfig` 并构造各 Cache 模块

## 3. 架构与约定
- **分层结构**：`global` → `cache_timing`（公共时序）→ 各 cache 段（`dc_cache`/`pc_cache`/`msipt_cache`/`pt_cache`/`walker_cache`）→ `statistics`
- **默认值合并**：每个 cache 段在解析前先拷贝一份 `common_cache_def`（来自 `cache_timing`），再按字段覆盖；缺失字段回退到硬编码默认（如 `num_sets=64, num_ways=4, replacement="plru"`）
- **Walker Cache 特殊处理**：`base_sets` 作为 C1/C2/C3 三级的共享基数，三级分别有独立 `num_ways` 与 `replacement` 覆盖
- **统计输出**：`statistics.output_file` 支持 `unified_log_file` 别名兼容旧路径；`task_trace_level` 支持 `off/basic/detail` 三种级别
- **时钟与随机种子**：`global.clock_period_ns` 控制 SystemC 时钟周期，`random_seed` 用于随机替换策略

## 4. 开发者应遵循的规则
- 新增 cache 或统计字段时，需在 `types.h` 中扩展对应结构体，并在 `json_config.cpp` 的 `parse_*_config` 中添加可选字段解析（保持向后兼容）
- 所有数值型配置项均通过 `j.contains(key)` 判断存在性后再赋值，避免未定义行为；新增字段也应遵循此模式
- 不要直接修改 `default_config.json` 中的硬编码默认值——应在 `parse_config` 中为每个 cache 段维护独立的 default 副本，以便不同 cache 拥有差异化默认
- 如需引入新的配置来源（环境变量、命令行参数），应在 `load_config` 之上增加一层合并逻辑，而非改动现有解析函数签名
- 配置变更需同步更新 `input_params_example.json` 以反映最新可用字段