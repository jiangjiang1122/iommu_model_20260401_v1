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
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/common/types.h
    - iommu/cache_src/subsystem/cache_subsystem.h
---

## 1. 系统概述

该仓库为 RISC-V IOMMU SystemC 性能与功能仿真模型，其**配置系统**专门服务于 `iommu/cache_src` 下的缓存子系统（DC/PC/MSIPT/PT/Dedup/Walker Cache），通过一个自实现的轻量 JSON 解析器加载外部 JSON 配置文件，将运行时参数注入到 `GlobalConfig` 结构体中，再由 `CacheSubsystem` 构造时消费。

## 2. 核心文件与职责

- **`iommu/cache_config/default_config.json`**：默认配置模板，定义全局时钟、随机种子、各 cache 几何（sets/ways/replacement）、时序延迟、Walker Cache 三级子表、失效策略（LIB/VN）及统计输出。
- **`iommu/cache_config/input_params_example.json`**：用户示例配置，展示可覆盖的子集字段。
- **`iommu/cache_src/common/mini_json.h`**：自实现 JSON 解析器 `mini_json::Parser`，支持对象、数组、字符串、布尔、数字、null，并允许 `//` 行注释和 `/* */` 块注释，无外部依赖。
- **`iommu/cache_src/common/json_config.h/.cpp`**：暴露 `load_config(json_path)` 与 `parse_config(json_str)` 两个入口；内部按 section 分派解析函数（如 `parse_cache_timing_config`、`parse_cache_config`）。
- **`iommu/cache_src/common/types.h`**：定义所有配置结构体——`CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig`，以及各 cache 的默认值（如 PT Cache 默认 `num_sets=1024, num_ways=8, replacement="srrip"`）。
- **`iommu/cache_src/subsystem/cache_subsystem.h`**：`CacheSubsystem` 构造函数签名 `CacheSubsystem(sc_module_name name, const GlobalConfig& cfg)`，从外部传入已解析的配置。

## 3. 架构与设计约定

### 3.1 分层解析 + 默认值合并
解析流程采用“默认值 → 部分覆盖”模式：每个 cache 先构造一个带硬编码默认值的 `CacheConfig`（例如 DC/PC/MSIPT 默认 `64 sets × 4 ways × plru`），再调用 `parse_cache_config(j["xxx_cache"], defaults)` 用 JSON 中的字段逐项覆盖。缺失的 key 不会报错，仅保留默认值。

### 3.2 共享 timing 配置
`cache_timing` 段提供通用延迟参数（arbiter/hash/read_set/compare/update_way_select/fill_compute_index/write_way/invalidation_compare_per_way），被 DC/PC/MSIPT/PT/Dedup/Walker 各 cache 作为公共基类默认值继承，避免重复声明。

### 3.3 Walker Cache 嵌套配置
`walker_cache` 使用嵌套对象 `ptw_c1 / ptw_c2 / ptw_c3`，每级独立指定 `num_ways`、`replacement`、`num_rams`、`ram_fifo_depth`，并通过 `base_sets` 统一控制各级 set 数。

### 3.4 Dedup Cache 几何复用
当 JSON 未显式给出 `dedup_cache` 时，解析器自动复用 `pt_cache` 的 `num_sets`/`num_ways`，并固定 `num_rams=4, ram_fifo_depth=8`，体现 dedup 与 PT Cache 在几何上的强耦合关系。

### 3.5 失效策略配置
`invalidation` 段集中管理失效通路开关：`enable`（总开关）、`lazy_enable`（是否走 LIB 延迟失效）、`lib_size`（LIB CAM 容量）、`vn_bits`（全局版本号位宽）、`lib_match_cycles`（匹配拍数）。这些字段直接映射到 `InvalidationConfig`，并被 `CacheSubsystem` 用于控制 CQ 桥接与懒失效行为。

### 3.6 统计与日志配置
`statistics` 段控制输出文件（支持 `output_file` 与兼容别名 `unified_log_file`）、延迟直方图开关、任务追踪级别（`off/basic/detail`）及直方图 bin 宽度。

## 4. 约束与规则

- **JSON 语法限制**：解析器仅支持标准 JSON 子集，但额外允许 `//` 单行注释和 `/* */` 块注释（见 `mini_json.h` 的 `skip_ws` 逻辑），因此配置文件可自由添加注释。
- **可选字段语义**：所有 JSON 字段均通过 `j.contains("key")` 判断存在性后再赋值；缺失字段即保持 `types.h` 中定义的默认值，不存在必填校验。
- **类型转换安全**：数值字段通过 `static_cast<uint32_t>(...)` 或 `static_cast<double>(...)` 转换，字符串字段通过 `operator std::string()` 读取；类型不匹配会抛出 `std::runtime_error`。
- **文件打开失败抛异常**：`load_config` 在无法打开文件时抛出 `std::runtime_error("Cannot open config file: ...")`，由调用方捕获。
- **配置不可变传播**：`GlobalConfig` 在解析完成后以 `const GlobalConfig&` 形式传递给 `CacheSubsystem`，子系统内只读访问，禁止运行时修改。
- **哈希模式回退**：`hash_mode` 支持 `"inval_v2"`（新哈希，支持 addr 枚举失效）与 `"legacy"`（旧哈希，仅回归对比），用于功能一致性验证。

## 5. 使用方式

典型用法为：
```cpp
#include "common/json_config.h"
auto cfg = iommu::load_config("iommu/cache_config/default_config.json");
iommu::CacheSubsystem subsystem("cache", cfg);
```
或通过 `parse_config(std::string)` 从内存字符串解析，便于测试与动态生成配置。

## 6. 适用范围说明

该配置系统**仅针对缓存子系统**（`iommu/cache_src`），IOMMU 顶层模块（`main.cpp`、`iommu_top.*`、`rp/`、`pcienoc/`、`ddr/`、`slink/`）通过 SystemC 端口绑定与硬编码参数运行，不使用此 JSON 配置机制。因此本配置系统是一个局部而非全局的运行时配置方案。
