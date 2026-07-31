---
kind: configuration_system
name: 基于 JSON 的 IOMMU 缓存子系统配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
---

该项目的配置系统围绕 IOMMU 性能模型的缓存子系统构建，采用轻量级 JSON 配置文件驱动架构，通过自定义 mini_json 解析器加载并验证参数。

## 核心架构

**配置数据结构**：GlobalConfig（types.h）作为顶层配置容器，包含时钟周期、随机种子、各类缓存配置（DC/PC/MSIPT/PT/Dedup/Walker）、失效处理配置和统计配置。每个 CacheConfig 结构体定义缓存几何参数（sets/ways/replacement策略）、时序延迟参数和多RAM支持。

**JSON 解析层**：json_config.cpp 提供 load_config() 和 parse_config() 两个接口，分别支持从文件路径和字符串加载配置。使用自实现的 mini_json 解析器，避免外部依赖。

**默认值机制**：解析器为每个缓存类型设置合理的默认值（如 DC/PC/MSIPT 默认为 64 sets × 4 ways × plru，PT 缓存为 1024 sets × 8 ways × srrip），允许用户仅覆盖需要的参数。

## 配置文件组织

**主配置文件**：iommu/cache_config/default_config.json 提供完整配置示例，包含所有可选字段。input_params_example.json 展示最小化配置用法。

**配置层次**：
- global：全局参数（时钟周期、随机种子）
- cache_timing：通用缓存时序参数（仲裁器延迟、哈希延迟、比较延迟等）
- 各缓存独立配置段（dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache）
- invalidation：失效处理开关和参数
- statistics：统计输出控制

## 集成方式

配置系统通过 CacheSubsystem 类在仿真启动时加载配置，将解析后的 GlobalConfig 传递给各个缓存模块构造函数。所有缓存实现（DCCache、PCCache、PTCache、WalkerCache等）都接受 CacheConfig 参数进行初始化。

## 约束与约定

- JSON 字段使用蛇形命名（snake_case）
- 数值类型自动转换（uint32_t/double）
- 缺失字段使用预定义默认值而非报错
- 支持嵌套对象（如 walker_cache.ptw_c1/c2/c3）
- 字符串枚举值（replacement: "plru"/"srrip"/"none"）