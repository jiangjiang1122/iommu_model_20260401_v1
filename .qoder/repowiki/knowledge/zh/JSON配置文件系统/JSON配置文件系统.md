---
kind: configuration_system
name: JSON配置文件系统
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
    - iommu/iommu_top.hh
---

该仓库采用基于 JSON 文件的配置系统，用于加载和分层管理 RISC-V IOMMU SystemC 性能模型的运行时参数。核心实现位于 `iommu/cache_src/common/` 目录下，通过自实现的轻量级 JSON 解析器完成配置文件的读取与解析。

**系统与工具**
- 自定义 mini_json 解析器（`mini_json.h`）：无外部依赖，支持对象、数组、字符串、布尔、数字、null 类型，以及 `//` 行注释和 `/* */` 块注释
- JSON 配置加载器（`json_config.h/.cpp`）：提供 `load_config()` 和 `parse_config()` 两个接口，分别支持从文件路径和字符串加载配置
- 顶层模块在 `iommu_top.hh` 中通过 `iommu::load_config("iommu/cache_config/default_config.json")` 加载默认配置

**配置文件结构**
配置文件采用嵌套 JSON 对象组织，主要包含以下层级：
- `global`：全局设置（时钟周期 `clock_period_ns`、随机种子 `random_seed`）
- `cache_timing`：缓存时序参数（仲裁器延迟、哈希延迟、读集延迟、比较延迟等）
- `dc_cache` / `pc_cache` / `msipt_cache` / `pt_cache`：四类缓存的几何参数（sets、ways、replacement策略、SRIP M位等）
- `dedup_cache`：去重缓存配置（继承 PT Cache 几何参数，默认4个RAM组）
- `walker_cache`：Walker 缓存三级结构（ptw_c1/c2/c3），共享 `base_sets` 基础大小
- `invalidation`：失效处理配置（总开关、延迟失效模式、LIB容量、VN位宽等）
- `statistics`：统计输出配置（输出文件、延迟直方图、任务追踪级别等）

**默认值与覆盖机制**
配置系统采用"默认值 + JSON覆盖"的分层策略：
- DC/PC/MSIPT 缓存默认 64 sets × 4 ways，PLRU 替换策略
- PT Cache 默认 1024 sets × 8 ways，SRIP 替换策略（M=2位）
- Dedup Cache 默认继承 PT Cache 几何参数，num_rams=4，ram_fifo_depth=8
- Walker Cache 三级默认：c1(1 way, none)、c2(2 ways, plru)、c3(4 ways, plru)
- 缺失字段自动使用默认值，允许部分覆盖（如仅修改 replacement 策略）

**配置类型定义**
所有配置结构体定义在 `types.h` 中：
- `CacheConfig`：通用缓存配置（几何+时序+哈希模式）
- `InvalidationConfig`：失效处理配置（enable/lazy_enable/lib_size/vn_bits/lib_match_cycles）
- `StatsConfig`：统计配置（output_file/enable_latency_histogram/enable_task_trace/task_trace_level/histogram_bin_width_ns）
- `GlobalConfig`：顶层配置聚合，包含上述所有子配置

**约束与验证**
- JSON 字段通过 `contains()` 检查可选性，未指定字段保持默认值
- 数值类型转换时进行显式 static_cast，确保类型安全
- 文件打开失败抛出 `std::runtime_error` 异常
- 支持两种哈希模式：`inval_v2`（新哈希，支持 addr 枚举失效）和 `legacy`（旧哈希，用于回归对比）