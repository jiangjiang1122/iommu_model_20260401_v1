---
kind: configuration_system
name: 基于JSON的缓存子系统配置加载机制
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/common/types.h
    - iommu/iommu_top.hh
---

## 1. 系统概述
该仓库采用**轻量级自定义 JSON 解析器**配合**结构化 C++ 配置对象**的方式管理运行时配置。配置系统主要服务于 `iommu/cache_src` 下的缓存仿真子系统（Cache Subsystem），用于动态调整缓存大小、替换策略、时序延迟以及统计输出行为。

核心特点：
- **零外部依赖**：内置 `mini_json.h` 实现 JSON 解析，无需引入 nlohmann/json 等第三方库。
- **分层配置结构**：通过 `GlobalConfig` 结构体统一管理全局参数、各层级缓存（DC/PC/PT/Walker）及统计模块配置。
- **默认值与覆盖机制**：在解析逻辑中硬编码了合理的默认值（如 `plru` 替换策略、64 sets），JSON 文件仅用于覆盖特定字段。

## 2. 关键文件与组件

| 文件路径 | 作用描述 |
| :--- | :--- |
| `iommu/cache_config/default_config.json` | **默认配置文件**。定义了仿真时钟周期、各级 Cache 的 Sets/Ways、替换算法及统计日志路径。 |
| `iommu/cache_src/common/json_config.h/cpp` | **配置加载接口**。提供 `load_config(path)` 从文件读取和 `parse_config(str)` 从字符串解析的功能。 |
| `iommu/cache_src/common/mini_json.h` | **底层解析引擎**。一个单头文件的极简 JSON 解析器，支持 Object, Array, String, Number, Bool, Null。 |
| `iommu/cache_src/common/types.h` | **配置数据模型**。定义了 `GlobalConfig`, `CacheConfig`, `StatsConfig` 等结构体，作为配置的内存载体。 |
| `iommu/iommu_top.hh` | **配置消费端**。在 `iommu_top` 模块构造时调用 `load_config` 初始化 `cfg` 成员，并传递给 `CacheSubsystem`。 |

## 3. 架构与约定

### 3.1 配置数据结构
配置被组织为嵌套的结构体：
- **`GlobalConfig`**: 根节点，包含 `clock_period_ns`（时钟周期）、`random_seed` 以及各子模块配置。
- **`CacheConfig`**: 通用缓存配置模板，包含 `num_sets`, `num_ways`, `replacement` ("plru"/"srrip"/"none") 以及精细的时序参数（如 `hash_latency_cycles`）。
- **`StatsConfig`**: 控制仿真日志的输出，包括文件名、是否启用延迟直方图、任务追踪级别等。

### 3.2 加载流程
1. **初始化**：`iommu_top` 构造函数中执行 `iommu::load_config("iommu/cache_config/default_config.json")`。
2. **解析**：`json_config.cpp` 读取文件内容，调用 `mini_json::Parser::parse` 生成内存树。
3. **映射**：通过 `parse_cache_config` 等辅助函数，将 JSON 键值对映射到 `CacheConfig` 成员。若 JSON 中缺失某字段，则保留结构体初始化时的默认值。
4. **注入**：解析完成的 `GlobalConfig` 对象被传入 `CacheSubsystem` 构造函数，完成仿真环境的参数化建模。

### 3.3 默认值策略
代码在 `json_config.cpp` 中为不同 Cache 类型预设了基准配置：
- **DC/PC/MSIPT Cache**: 默认 64 sets, 4 ways, PLRU。
- **PT Cache**: 默认 1024 sets, 8 ways, SRRIP (m=2)。
- **Walker Cache**: 采用三级结构 (PTWc_1/2/3)，默认 Ways 分别为 1, 2, 4。

## 4. 开发者指南

### 4.1 修改配置
若要调整仿真参数，请直接编辑 `iommu/cache_config/default_config.json` 或创建新的 JSON 文件并在 `iommu_top.hh` 中修改加载路径。支持的配置项参考 `input_params_example.json`。

### 4.2 扩展配置项
1. 在 `types.h` 的对应结构体（如 `CacheConfig`）中添加新成员变量并赋予默认值。
2. 在 `json_config.cpp` 的 `parse_cache_config` 或相关解析函数中，增加 `if (j.contains("key"))` 分支以读取新字段。
3. 确保 `mini_json.h` 能正确处理新字段的类型（目前支持 int, double, string, bool）。

### 4.3 注意事项
- **路径依赖**：当前配置加载使用相对路径 `"iommu/cache_config/default_config.json"`，请确保仿真程序的工作目录（Working Directory）位于项目根目录。
- **类型安全**：`mini_json` 在类型不匹配时会抛出 `std::runtime_error`，请确保 JSON 中的数值类型（整型/浮点型）与 C++ 结构体定义一致。
- **硬编码默认值**：部分默认值（如 PT Cache 的 `srrip` 策略）是在解析函数中硬编码的，修改时需同步检查 `json_config.cpp` 逻辑。