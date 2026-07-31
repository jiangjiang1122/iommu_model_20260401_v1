---
kind: configuration_system
name: IOMMU 仿真配置系统：JSON 驱动与编译期参数混合机制
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/iommu_top.hh
    - iommu/iommu_perf_model/iommu_perf_params.hh
---

## 1. 系统概述
该 RISC-V IOMMU 仿真平台采用**混合配置策略**，将运行时动态配置（JSON 文件）与编译期静态参数（C++ 头文件常量）相结合。这种设计旨在兼顾缓存微架构参数的灵活调整与高性能仿真内核的稳定性。

- **运行时配置**：主要针对 Cache 子系统（容量、路数、替换策略、时序延迟），通过 JSON 文件加载。
- **编译期配置**：主要针对流水线深度、FIFO 缓冲大小、并发限制及功能开关，通过 `#define` 宏在编译时确定。

## 2. 核心组件与关键文件

### 2.1 配置文件 (JSON)
位于 `iommu/cache_config/` 目录下：
- **`default_config.json`**：默认配置模板，定义了全局时钟周期、各层级 Cache（DC, PC, PT, MSIPT, Walker）的物理参数以及统计日志输出路径。
- **`input_params_example.json`**：用于不同测试场景的参数示例。

### 2.2 配置解析引擎
位于 `iommu/cache_src/common/` 目录下：
- **`json_config.h/cpp`**：提供 `load_config` 和 `parse_config` 接口。它依赖一个轻量级的内部 JSON 解析器 `mini_json.h`，避免了引入第三方重型库。
- **`types.h`**：定义了配置数据的内存结构体 `GlobalConfig`、`CacheConfig` 和 `StatsConfig`，作为解析结果的载体。

### 2.3 编译期参数定义
- **`iommu_perf_params.hh`**：集中定义了仿真模型的“硬约束”参数，如 FIFO 深度 (`FIFO_DEPTH_*`)、AXI 端口带宽、DDR 延迟以及预取深度 (`PT_DEDUP_PREFETCH_DEPTH`)。

## 3. 架构与约定

### 3.1 配置加载流程
1. **初始化**：在 `iommu_top` 模块构造时，自动调用 `iommu::load_config("iommu/cache_config/default_config.json")`。
2. **解析与映射**：解析器读取 JSON 内容，将其映射到 `iommu::GlobalConfig` 结构体。若 JSON 中缺少某项，则使用代码中预设的默认值。
3. **实例化**：配置对象被传递给 `CacheSubsystem` 构造函数，用于初始化各个 Cache 模块的 Set/Way 结构和时序模型。

### 3.2 参数分层逻辑
- **微架构层 (Micro-architecture)**：Cache 的 `num_sets`, `num_ways`, `replacement` (plru/srrip) 等直接影响命中率模拟的参数，放在 JSON 中以便快速迭代实验。
- **系统互联层 (Interconnect)**：NoC 延迟、AXI ID 池大小、Outstanding 上限等涉及系统稳定性的参数，放在 `iommu_perf_params.hh` 中，防止运行时误配导致仿真死锁或崩溃。

## 4. 开发者指南

### 4.1 如何修改 Cache 参数
若需调整 Cache 性能表现，直接编辑 `iommu/cache_config/default_config.json`。例如，修改 PT Cache 的路数：
```json
"pt_cache": {
  "num_sets": 1024,
  "num_ways": 16,  // 从 8 改为 16
  "replacement": "srrip"
}
```

### 4.2 如何调整流水线深度或并发度
若需修改 FIFO 深度或最大并发任务数，必须编辑 `iommu/iommu_perf_model/iommu_perf_params.hh` 并重新编译项目。注意保持 `DDR_MAX_OUTSTANDING` 大于所有 Walker 并发请求之和。

### 4.3 扩展新配置项
1. 在 `types.h` 的对应 `struct` 中添加字段并设置默认值。
2. 在 `json_config.cpp` 的 `parse_config` 函数中添加 `if (j.contains("key"))` 逻辑进行提取。
3. 在 JSON 文件中添加对应的键值对。