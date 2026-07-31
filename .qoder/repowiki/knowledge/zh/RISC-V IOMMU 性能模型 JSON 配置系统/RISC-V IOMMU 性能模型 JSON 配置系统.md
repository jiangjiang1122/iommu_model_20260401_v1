---
kind: configuration_system
name: RISC-V IOMMU 性能模型 JSON 配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/common/types.h
    - iommu/iommu_top.hh
---

该仓库为基于 SystemC 的 RISC-V IOMMU 完整性能模型，其配置系统采用**纯 C++ 自实现的轻量级 JSON 解析器**，通过 JSON 文件加载并驱动缓存子系统（DC/PC/PT/MSIPT/Walker Cache）及统计行为的全部运行时参数。

## 1. 系统与框架
- **JSON 解析器**：`iommu/cache_src/common/mini_json.h` 提供零依赖的 `mini_json::Value` + `Parser`，支持对象、数组、字符串、布尔、整数、浮点、null 以及 `//` 和 `/* */` 注释。
- **配置加载层**：`json_config.h/.cpp` 暴露 `load_config(json_path)` 与 `parse_config(json_str)` 两个入口，将 JSON 映射到 `GlobalConfig` 结构体。
- **顶层集成**：`iommu_top.hh` 在模块构造时直接调用 `iommu::load_config("iommu/cache_config/default_config.json")` 初始化 `cfg`，再传入 `CacheSubsystem` 构造函数。

## 2. 关键文件与包
- `iommu/cache_config/default_config.json` — 默认配置（含全局时钟、各 cache 几何/替换策略、Walker 三级子表、统计输出等）
- `iommu/cache_config/input_params_example.json` — 示例配置（演示可选字段覆盖）
- `iommu/cache_src/common/json_config.h/.cpp` — JSON→GlobalConfig 解析逻辑
- `iommu/cache_src/common/mini_json.h` — 内嵌 JSON 解析器
- `iommu/cache_src/common/types.h` — `CacheConfig` / `StatsConfig` / `GlobalConfig` 结构体定义
- `iommu/iommu_top.hh` — 顶层模块中加载配置并构造 `CacheSubsystem`

## 3. 架构与约定
- **分层结构**：`mini_json`（词法/语法解析）→ `json_config`（字段映射+默认值合并）→ `GlobalConfig`（强类型结构体）→ `CacheSubsystem`（消费配置构建各 cache 实例）。
- **默认值合并策略**：每个 cache 段先以 `common_cache_def` 为基础，再按具体 cache 设定默认几何（如 dc/pc/msipt=64×4 plru，pt=1024×8 srrip），最后用 JSON 中的可选字段覆盖；缺失字段保持默认，不报错。
- **Walker Cache 特殊处理**：`walker_cache.base_sets` 作为 ptw_c1/c2/c3 的共享 sets 基数，每级独立设置 ways/replacement/ram 参数。
- **Dedup Cache**：默认复用 pt_cache 的 num_sets/num_ways，固定 num_rams=4、ram_fifo_depth=8，可通过 `dedup_cache` 段覆盖。
- **统计配置**：`statistics` 段控制输出文件、延迟直方图开关、task trace 级别与直方图 bin 宽度。

## 4. 约定与约束
- **JSON 键名必须与代码硬编码一致**：`json_config.cpp` 使用 `j.contains("...")` 逐项检查，新增字段需同步修改解析器。
- **可选字段安全访问**：所有字段读取前均做 `contains` 检查，未提供的字段走默认值路径，不会抛异常。
- **文件打开失败抛出异常**：`load_config` 在无法打开文件时抛出 `std::runtime_error`。
- **无环境变量/命令行覆盖机制**：当前仅支持从固定路径的 JSON 文件加载，未见 `.env`、`--config` 或环境变量注入。
- **配置即数据**：`GlobalConfig` 是纯 POD 结构体，不含行为逻辑，便于序列化/调试打印。