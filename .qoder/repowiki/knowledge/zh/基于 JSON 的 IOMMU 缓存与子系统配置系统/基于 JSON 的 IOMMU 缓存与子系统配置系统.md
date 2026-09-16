---
kind: configuration_system
name: 基于 JSON 的 IOMMU 缓存与子系统配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/types.h
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/iommu_top.hh
    - main.cpp
---

## 1. 采用的方案

本仓库使用**纯 C++ + 自研 mini-JSON 解析器**实现配置加载，没有引入外部配置库（如 Boost.PropertyTree、nlohmann/json 等）。核心思路是：
- 用 `iommu::GlobalConfig` 结构体集中描述所有可配置参数；
- 通过 `iommu::load_config(path)` / `parse_config(json_str)` 从 JSON 文件/字符串解析为 `GlobalConfig`；
- `iommu_top` 在构造时硬编码调用 `load_config("iommu/cache_config/default_config.json")` 完成启动期配置装载。

该配置系统主要服务于 **IOMMU 性能模型中的 Cache 子系统**（DC/PC/PT/MSIPT/Walker/Dedup Cache）以及失效处理、统计输出等。它不用于 SystemC 顶层模块（main.cpp）的参数传递——`sc_main` 仅创建模块并绑定 TLM 端口，仿真时长固定为 300ms。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `iommu/cache_src/common/types.h` | 定义 `CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig` 等全部配置结构体及默认值 |
| `iommu/cache_src/common/json_config.h/.cpp` | JSON 解析入口：`load_config()`、`parse_config()`，内部依赖 `mini_json.h` |
| `iommu/cache_src/common/mini_json.h` | 极简 JSON 解析器（Value/Parser），供配置解析使用 |
| `iommu/cache_config/default_config.json` | 完整的默认配置文件，覆盖 global / cache_timing / dc_cache / pc_cache / msipt_cache / pt_cache / dedup_cache / walker_cache / invalidation / statistics 各段 |
| `iommu/cache_config/input_params_example.json` | 示例配置文件（精简版），演示可选字段 |
| `iommu/iommu_top.hh` | `iommu_top` 类成员 `cfg` 在初始化列表中调用 `load_config(...)` 加载配置，并将 `cfg` 传给 `CacheSubsystem` |
| `main.cpp` | SystemC 入口，仅负责实例化模块和 TLM 绑定，不包含任何命令行参数或环境变量读取逻辑 |

## 3. 架构与约定

### 3.1 配置结构分层
`GlobalConfig` 是根节点，包含：
- `global`：`clock_period_ns`、`random_seed`；
- `cache_timing`：被 DC/PC/MSIPT/PT/Walker 各 cache 共享的时序延迟字段（arbiter/hash/read_set/compare/update_way_select/fill_compute_index_*/write_way/invalidation_compare_per_way）；
- 各 cache 独立配置：`dc_cache`、`pc_cache`、`msipt_cache`、`pt_cache`、`dedup_cache`、`walker_ptw_c1/c2/c3`；
- `invalidation`：失效总开关、延迟失效（LIB/VN）、LIB 容量、VN 位宽、匹配周期；
- `statistics`：输出文件、延迟直方图开关、task trace 级别、直方图 bin 宽度。

每个子 cache 的 `CacheConfig` 都有**内置默认值**（例如 PT cache 默认 `num_sets=1024, num_ways=8, replacement="srrip", srrip_m_bits=2`；Dedup cache 默认几何复用 PT cache 且 `num_rams=4, ram_fifo_depth=8`），JSON 中只写需要覆盖的字段即可。

### 3.2 解析策略：部分覆盖 + 默认回退
`parse_config()` 对每个 JSON 段采用 `if (j.contains("xxx"))` 判断后赋值，未出现的键保持 `CacheConfig` 默认值。对于 Walker Cache，还会根据 `base_sets` 派生 C1/C2/C3 的 `num_sets`。这种设计允许用户只提供增量配置。

### 3.3 加载路径
`iommu_top` 构造函数中直接硬编码路径：
```cpp
iommu::GlobalConfig cfg = iommu::load_config("iommu/cache_config/default_config.json");
```
因此当前运行时的配置来源**唯一**是相对工作目录下的 `iommu/cache_config/default_config.json`。`input_params_example.json` 目前仅作为文档/参考，未被代码引用。

### 3.4 配置到运行时对象的传递
`iommu_top` 持有 `cfg` 成员并在构造时将其传入 `iommu::CacheSubsystem cache_sub{"cache_sub", cfg}`，由 CacheSubsystem 进一步分发给各具体 cache 模块。因此配置系统的边界止于 `GlobalConfig` 对象，后续模块通过构造函数参数消费。

## 4. 约定与约束

- **配置格式**：必须为合法的 JSON，键名与 `json_config.cpp` 中 `contains` 检查一一对应；新增字段需同时修改解析函数。
- **缺失字段语义**：未提供的字段一律使用 `types.h` 中结构体的默认值，不会报错退出。
- **文件打开失败**：`load_config()` 在无法打开文件时抛出 `std::runtime_error`，属于致命错误。
- **无运行时热更新**：配置仅在 `iommu_top` 构造时加载一次，仿真期间不可更改。
- **无命令行/环境变量覆盖**：`main.cpp` 的 `sc_main` 不接受任何参数，也没有 `getenv` 调用；当前不存在命令行开关来指定配置文件路径。
- **统计输出目标**：`statistics.output_file` 默认 `cache_sim.log`，可通过 JSON 覆盖；若未设置则回退到 `unified_log_file`（兼容旧字段）。
- **替换策略字符串**：`replacement` 字段支持 `plru`、`srrip`、`none` 等，由替换策略模块按字符串选择实现。
- **哈希模式**：`hash_mode` 支持 `inval_v2`（新哈希，支持 addr 枚举失效）和 `legacy`（旧哈希，用于回归对比）。
- **多 RAM 配置**：`num_rams` 必须是 2 的幂且整除 `num_sets`（见 `CacheConfig` 注释），否则下游 cache 行为未定义。
- **Walker Cache 层级耦合**：C1/C2/C3 的 `num_sets` 由 `walker_base_sets` 统一控制，不应单独设置不同值。

## 5. 与其他部分的交互

- 该配置系统**仅影响 IOMMU 性能模型中的缓存子系统**，不影响 SystemC 顶层拓扑（main.cpp 中模块实例化和 TLM 绑定是硬编码的）。
- 测试脚本（`test_*.sh`、`compile_and_test.sh` 等）位于仓库根目录，但未见其向进程传递配置文件路径；实际运行时依赖工作目录下存在 `iommu/cache_config/default_config.json`。
- 分析脚本（`tmp/*.py`、`tmp/*.sh`）读取的是仿真输出的日志/CSV，而非配置文件本身。

## 6. 已知局限

- 配置路径硬编码，无法通过命令行或环境变量切换；如需支持多场景配置，需在 `main.cpp` 或 `iommu_top` 增加参数解析。
- 没有 schema 校验，JSON 拼写错误或类型不匹配会在解析阶段静默忽略（`contains` 失败）或抛异常（类型转换失败）。
- 没有版本化的配置迁移机制，新增字段需手动维护向后兼容。
