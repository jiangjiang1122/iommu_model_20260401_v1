---
kind: configuration_system
name: 基于内嵌 mini-json 的 JSON 配置文件系统（缓存/失效/统计参数）
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

## 1. 系统概览

本项目为 RISC-V IOMMU SystemC 仿真模型，其运行时配置完全通过 **JSON 文件** 加载，由自实现的轻量级 JSON 解析器 `mini_json` 提供，不依赖任何外部库。配置入口位于 `iommu/cache_src/common/json_config.{h,cpp}`，对外暴露两个函数：
- `load_config(json_path)`：从文件路径读取并解析
- `parse_config(json_str)`：从字符串解析

两者最终都调用 `mini_json::Parser::parse` 将 JSON 文本解析为 `mini_json::Value` 树，再映射到 `GlobalConfig` 结构体。

## 2. 关键文件与包

| 文件 | 作用 |
|---|---|
| `iommu/cache_src/common/json_config.h/.cpp` | 配置加载与 JSON→结构体映射核心逻辑 |
| `iommu/cache_src/common/mini_json.h` | 自实现 JSON 解析器（支持对象、数组、字符串、数字、布尔、null，以及 `//` 和 `/* */` 注释） |
| `iommu/cache_src/common/types.h` | 定义 `GlobalConfig`、`CacheConfig`、`InvalidationConfig`、`StatsConfig` 等配置结构体及其默认值 |
| `iommu/cache_config/default_config.json` | 完整默认配置模板（含 DC/PC/MSIPT/PT/Walker Cache、去重缓存、失效、统计） |
| `iommu/cache_config/input_params_example.json` | 精简示例配置 |
| `Makefile` | 将 `json_config.cpp` 纳入编译目标 |

## 3. 架构与设计约定

### 3.1 分层解析 + 默认值覆盖
每个 cache 段在解析前先构造一个带默认值的 `CacheConfig` 实例，再用 JSON 中存在的字段进行覆盖。例如：
- `dc_cache` / `pc_cache` / `msipt_cache` 默认 `num_sets=64, num_ways=4, replacement="plru"`
- `pt_cache` 默认 `num_sets=1024, num_ways=8, replacement="srrip", srrip_m_bits=2`
- `dedup_cache` 未显式配置时几何复用 `pt_cache`，且强制 `num_rams=4, ram_fifo_depth=8`
- Walker Cache 三段 `ptw_c1/c2/c3` 分别默认 `ways=1/2/4`、`replacement=none/plru/plru`

这种“先设默认、再按需覆盖”的模式使 JSON 可以只声明需要变更的参数。

### 3.2 共享时序参数
`cache_timing` 段定义一组跨所有 cache 共享的延迟周期（如 `arbiter_latency_cycles`、`hash_latency_cycles`、`read_set_latency_cycles`、`compare_latency_cycles`、`fill_compute_index_*_cycles`、`write_way_latency_cycles`、`invalidation_compare_per_way_cycles`），各 cache 可单独覆盖 `read_set_latency_cycles` 等个别字段。

### 3.3 可选段与向后兼容
解析器使用 `j.contains("...")` 判断段是否存在，缺失段即跳过。新增配置项只需添加新的 `contains` 分支，不会破坏旧 JSON。

### 3.4 统计输出文件别名
`statistics.output_file` 与 `statistics.unified_log_file` 是同一字段的两个别名——若同时出现，`output_file` 优先；否则回退到 `unified_log_file`。

### 3.5 失效子系统配置
`invalidation` 段控制失效总开关、延迟失效（LIB/VN）、LIB 容量 (`lib_size`)、全局版本号位宽 (`vn_bits`) 及 CAM 匹配延迟 (`lib_match_cycles`)。这些字段直接写入 `GlobalConfig.invalidation`。

## 4. 约束与规则

- **JSON 格式**：必须为标准 JSON，但解析器额外支持 `//` 行注释与 `/* */` 块注释（见 `mini_json.h` 的 `skip_ws`）。
- **键存在性检查**：所有字段读取前均用 `contains` 判断，缺失键不会抛异常，仅保留默认值。
- **类型转换**：数值字段通过 `static_cast<uint32_t>` / `double` 转换，类型不匹配会在 `mini_json` 的 `operator T()` 中抛出 `std::runtime_error`。
- **文件打开失败**：`load_config` 在无法打开文件时抛出 `std::runtime_error("Cannot open config file: ...")`。
- **全局时钟与随机种子**：`global.clock_period_ns` 与 `global.random_seed` 是全局配置，影响仿真时间步长与随机数生成。
- **哈希模式**：`CacheConfig.hash_mode` 默认 `"inval_v2"`，支持回退到 `"legacy"` 用于回归对比。
- **替换策略**：`CacheConfig.replacement` 接受字符串（如 `"plru"`、`"srrip"`、`"none"`），具体策略由 `cache/replacement/` 下的策略类实现。
- **多 RAM 配置**：`num_rams` 必须是 2 的幂且需整除 `num_sets`（见 `types.h` 注释），`ram_fifo_depth` 控制每组 RAM 前置 FIFO 深度。

## 5. 使用方式

典型流程：
1. 编辑 `iommu/cache_config/*.json` 中的参数
2. 在顶层或子系统代码中调用 `iommu::load_config("iommu/cache_config/default_config.json")` 获取 `GlobalConfig`
3. 将 `GlobalConfig` 注入到各 cache 子系统（DC/PC/MSIPT/PT/Walker/Dedup）
4. 运行仿真，统计输出写入 `statistics.output_file` 指定的日志文件

该配置系统仅覆盖缓存/失效/统计相关参数；IOMMU 功能行为（如地址翻译阶段、寄存器初始值等）由其他头文件（如 `iommu/include/iommu_registers.hh`、`param_trans_def.hh`）管理，不在本配置体系范围内。