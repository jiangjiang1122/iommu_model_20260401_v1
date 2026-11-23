---
kind: configuration_system
name: 基于JSON的缓存子系统配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/cache_src/subsystem/cache_subsystem.h
    - iommu/iommu_top.hh
---

## 1. 采用的方案

本仓库使用自研的轻量级 JSON 配置加载器（`iommu/cache_src/common/mini_json.h` + `json_config.cpp`）来加载 RISC-V IOMMU 性能仿真中 cache subsystem 的全部运行时参数。没有引入外部配置库，也没有环境变量或命令行参数解析；所有可配置项集中在 `iommu/cache_config/` 下的 JSON 文件中。

## 2. 关键文件与包

- `iommu/cache_config/default_config.json`：默认完整配置模板，包含 global、cache_timing、dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache、statistics、invalidation 全部字段。
- `iommu/cache_config/input_params_example.json`：精简版示例，仅含必要字段，用于演示最小配置集。
- `iommu/cache_src/common/json_config.h` / `json_config.cpp`：暴露 `load_config(json_path)` 和 `parse_config(json_str)` 两个入口，内部依赖 `mini_json::Parser`。
- `iommu/cache_src/common/types.h`：定义配置结构体 `CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig`，是 JSON 到 C++ 对象的映射目标。
- `iommu/cache_src/subsystem/cache_subsystem.h` / `.cpp`：`CacheSubsystem` 模块构造函数签名 `CacheSubsystem(sc_module_name, const GlobalConfig&)`，消费 `GlobalConfig` 实例化各 cache 子模块。
- `iommu/iommu_top.hh`：顶层模块在成员初始化列表中调用 `iommu::load_config("iommu/cache_config/default_config.json")` 并构造 `iommu::CacheSubsystem cache_sub{"cache_sub", cfg}`。

## 3. 架构与约定

### 3.1 配置结构分层

`GlobalConfig` 作为根对象，聚合以下子配置：

| JSON 顶层键 | 对应结构体 | 作用 |
|---|---|---|
| `global` | `clock_period_ns`, `random_seed` | 全局时钟周期与随机种子 |
| `cache_timing` | 共享给各 cache 的时序延迟字段 | arbiter/hash/read_set/compare/fill/write/invalidation 等拍数 |
| `dc_cache` / `pc_cache` / `msipt_cache` / `pt_cache` / `dedup_cache` | `CacheConfig` | 各 cache 几何（sets/ways/replacement/srrip_m_bits/num_rams/ram_fifo_depth）+ 覆盖的时序 |
| `walker_cache.base_sets` + `walker_cache.ptw_c1/c2/c3` | `uint32_t walker_base_sets` + 三个 `CacheConfig` | Walker Cache 三级子表独立配置 |
| `invalidation` | `InvalidationConfig` | 失效总开关、lazy_enable、LIB大小、VN位宽、匹配延时 |
| `statistics` | `StatsConfig` | 输出文件名、直方图开关、task trace level、bin宽度 |

### 3.2 默认值与覆盖策略

`parse_cache_config` 采用 defaults + override 模式：对每个 cache 先构造一个带硬编码默认值的 `CacheConfig`（例如 dc/pc/msipt 默认 num_sets=64, num_ways=4, replacement="plru"；pt_cache 默认 num_sets=1024, num_ways=8, replacement="srrip", srrip_m_bits=2），再逐项用 JSON 中的字段覆盖。缺失字段保留默认值，因此 JSON 可以只写差异部分。

`dedup_cache` 的默认几何复用 `cfg.pt_cache`（即继承 pt_cache 的 sets/ways），并固定 num_rams=4, ram_fifo_depth=8。

Walker Cache 的三个级别（c1/c2/c3）各自有独立的默认 ways（1/2/4）和 replacement（none/plru/plru），且强制把 num_sets 对齐到 walker_base_sets。

### 3.3 解析实现要点

- 所有字段读取都通过 j.contains("key") 做可选性判断，然后 static_cast<T>(j["key"]) 取值，不存在时跳过——这是向后兼容新增字段的约定。
- `load_config` 打开文件后读入整个文本，委托给 `parse_config`；文件打不开时抛出 std::runtime_error("Cannot open config file: ...")。
- `statistics.output_file` 支持旧字段名 unified_log_file 作为别名：若同时出现，优先取 output_file。

### 3.4 启动链路

```text
iommu_top.hh 成员初始化列表
  → iommu::load_config("iommu/cache_config/default_config.json")
  → mini_json::Parser::parse(json_str)
  → parse_config() 构建 GlobalConfig
  → iommu::CacheSubsystem cache_sub{"cache_sub", cfg}
```

`CacheSubsystem` 构造函数接收 `const GlobalConfig& cfg`，并通过 `config()` 访问器对外暴露只读引用，供统计、报告和多 RAM 拓扑（pt_num_rams_、dedup_num_rams_、walker_num_rams_）使用。

### 3.5 运行时开关

`invalidation.enable` 控制整条失效通路：当为 false 时，`CacheSubsystem::invalidation_enabled()` 返回 false，`perf_enqueue_cache_invalidation`（位于 `iommu_top.cc`）会直接 return，CQ→性能模型失效桥接不生效。`invalidation.lazy_enable` 控制是否走 LIB/VN 延迟失效路径。

## 4. 约定与约束

- **配置文件位置约定**：顶层 `iommu_top.hh` 硬编码了默认路径 `"iommu/cache_config/default_config.json"`，运行期必须保证该相对路径存在，否则 `load_config` 抛异常。
- **JSON 字段可选性约定**：所有字段均通过 contains 检查，新增字段不会破坏旧 JSON；但新增字段必须在 `parse_config` 中显式添加解析分支，否则会被静默忽略。
- **默认值来源约定**：默认值不是从 JSON 中读取，而是写在 `parse_config` 里（例如 dc/pc/msipt 的 64×4 plru、pt_cache 的 1024×8 srrip）。修改默认行为需要改代码而非只改 JSON。
- **类型安全约定**：JSON 数值一律通过 static_cast<uint32_t> / static_cast<double> / static_cast<bool> 转换，无 schema 校验；非法类型会在运行时抛出 mini_json 解析异常。
- **向后兼容字段约定**：`statistics.unified_log_file` 作为 `output_file` 的别名保留，但 `output_file` 优先级更高。
- **配置与功能解耦**：`GlobalConfig` 仅承载数据，业务逻辑通过 `CacheSubsystem::config()` 只读访问，避免在解析层耦合仿真语义。
- **不在本系统中处理的配置**：环境变量、命令行参数、feature flags、secrets 在本仓库中未见相关机制；配置完全由 JSON 文件驱动。