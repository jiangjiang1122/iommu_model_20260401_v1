---
kind: configuration_system
name: IOMMU 性能模型配置系统：JSON 驱动与编译期宏混合机制
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/iommu_perf_model/iommu_perf_params.hh
    - Makefile
    - iommu/iommu_top.hh
---

## 1. 系统概述
该 RISC-V IOMMU SystemC 性能模型采用**双层配置架构**，以支持灵活的缓存仿真与流水线行为调整：
1. **运行时 JSON 配置**：主要用于缓存子系统（Cache Subsystem），包括各级缓存（DC/PC/PT/Walker）的几何结构、替换策略、时序延迟以及统计输出选项。
2. **编译期宏定义配置**：主要用于性能模型核心（Perf Model），通过 `Makefile` 传递 `-D` 标志控制 FIFO 深度、并发限制、功能开关（如去重、预取、Walker Cache）等关键架构参数。

## 2. 核心组件与文件
### 2.1 JSON 配置层 (Cache Subsystem)
*   **配置文件**：
    *   `iommu/cache_config/default_config.json`：默认配置，定义了全局时钟周期、各缓存的 Set/Way 数、替换算法（PLRU/SRRIP）及详细到 Cycle 级的时序参数。
    *   `iommu/cache_config/input_params_example.json`：示例配置，展示了如何调整统计日志输出和任务追踪级别。
*   **解析逻辑**：
    *   `iommu/cache_src/common/json_config.h/cpp`：提供 `load_config` 和 `parse_config` 接口。使用自研的轻量级 `mini_json` 库进行解析，避免引入外部重型依赖。
    *   `iommu/cache_src/common/types.h`：定义了 `GlobalConfig`、`CacheConfig` 和 `StatsConfig` 结构体，作为配置数据的内存载体。
*   **加载入口**：在 `iommu/iommu_top.hh` 中，顶层模块初始化时直接调用 `iommu::load_config("iommu/cache_config/default_config.json")` 获取全局配置。

### 2.2 编译期宏配置层 (Perf Model)
*   **参数定义**：
    *   `iommu/iommu_perf_model/iommu_perf_params.hh`：集中定义了所有硬编码的性能参数，如 `FIFO_DEPTH_*`（FIFO 深度）、`DDR_READ_LATENCY`（DDR 延迟）、`AXI_MASTER_*_BANDWIDTH_MBPS`（带宽）等。
*   **动态覆盖机制**：
    *   部分关键布尔值或整数参数（如 `PTW_WALKER_CACHE_ENABLED`、`PT_DEDUP_PREFETCH_DEPTH`）通过 `#ifndef TEST_CFG_...` 预处理指令暴露给编译器。
    *   `Makefile` 中的 `TEST_FLAGS` 变量根据选择的测试场景（如 `seq128k_twostage`）自动注入相应的宏定义，实现不同实验场景的快速切换。

## 3. 架构设计与约定
*   **分层解耦**：缓存微架构参数（Micro-architecture）与系统互联参数（Interconnect/System）分离。前者通过 JSON 灵活调整以进行敏感性分析，后者通过头文件常量保证仿真核心的稳定性与性能。
*   **默认值与覆盖**：JSON 解析器采用“默认值 + 覆盖”模式。`parse_config` 函数内部为每个缓存类型预设了基准值（如 DC Cache 默认为 64 Sets, 4 Ways），仅当 JSON 中存在对应键时才进行更新，确保了配置的健壮性。
*   **时序建模精度**：JSON 配置细化到了缓存访问的各个子阶段（如 `hash_latency_cycles`, `compare_latency_cycles`），允许研究人员精确模拟不同硬件实现带来的时序差异。

## 4. 开发者指南
*   **修改缓存参数**：直接编辑 `iommu/cache_config/default_config.json`。修改后无需重新编译，重启仿真即可生效。
*   **调整系统性能参数**：修改 `iommu/iommu_perf_model/iommu_perf_params.hh` 中的常量。若需在不同场景间对比，建议在 `Makefile` 中新增 `TEST` 分支并定义对应的 `TEST_FLAGS`。
*   **添加新配置项**：
    1. 在 `types.h` 的对应 `struct` 中添加成员。
    2. 在 `json_config.cpp` 的 `parse_config` 中添加解析逻辑。
    3. 在 JSON 模板文件中补充示例字段。
*   **注意事项**：目前 JSON 配置主要作用于 `cache_src` 目录下的仿真模块。`iommu_perf_model` 中的大部分 FIFO 深度和并发限制仍为静态常量，修改这些参数需要重新编译整个项目。