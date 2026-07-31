---
kind: configuration_system
name: 基于JSON的缓存参数配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/iommu_top.hh
---

该RISC-V IOMMU SystemC仿真模型采用轻量级JSON配置文件驱动的配置系统，专门用于管理多级Cache（DC/PC/MSIPT/PT/Dedup/Walker）及统计行为的运行时参数。

**系统与工具**
- 使用自实现的极简JSON解析器 `mini_json`（位于 `iommu/cache_src/common/mini_json.h`），避免引入外部依赖
- 配置结构体定义在 `iommu/cache_src/common/types.h` 中，包含 `GlobalConfig`、`CacheConfig`、`StatsConfig` 等核心类型
- JSON文件由 `json_config.cpp` 中的 `load_config()` 和 `parse_config()` 函数加载解析

**核心文件与位置**
- 配置文件：`iommu/cache_config/default_config.json`（默认配置）、`iommu/cache_config/input_params_example.json`（示例覆盖配置）
- 解析逻辑：`iommu/cache_src/common/json_config.h/.cpp`
- 类型定义：`iommu/cache_src/common/types.h`（第597-643行定义了所有配置结构体）
- 顶层集成：`iommu/iommu_top.hh` 第60行通过 `iommu::load_config("iommu/cache_config/default_config.json")` 加载配置

**架构设计**
1. **分层配置结构**：`GlobalConfig` 聚合全局设置（时钟周期、随机种子）、各Cache独立配置（`dc_cache`、`pc_cache`、`msipt_cache`、`pt_cache`、`dedup_cache`）、Walker Cache三级配置（`walker_ptw_c1/c2/c3`）和统计配置
2. **默认值继承机制**：每个Cache配置都从 `common_cache_def` 继承公共时序参数，再按特定Cache设定默认几何参数（如PT Cache默认1024x8，Dedup Cache复用PT Cache几何并设置num_rams=4）
3. **可选字段支持**：解析器使用 `j.contains()` 检查字段存在性，未指定的字段保持默认值，允许部分覆盖
4. **Walker Cache特殊处理**：base_sets作为基础参数，各level的num_sets强制设为base_sets值

**配置项分类**
- 全局参数：`clock_period_ns`、`random_seed`
- 通用时序：仲裁器延迟、哈希延迟、读取集延迟、比较延迟、替换算法选择（plru/srrip/none）
- Cache几何：`num_sets`、`num_ways`、`srrip_m_bits`、多RAM配置的 `num_rams` 和 `ram_fifo_depth`
- 统计控制：输出文件路径、延迟直方图开关、任务跟踪级别（off/basic/detail）

**约束与约定**
- JSON格式必须严格匹配预定义的键名，解析器不验证数值范围
- Dedup Cache默认继承PT Cache的几何参数，但强制设置 `num_rams=4` 和 `ram_fifo_depth=8`
- Walker Cache各level的 `num_sets` 会被强制覆写为 `walker_base_sets` 的值
- 配置文件路径硬编码在 `iommu_top.hh` 中，未提供命令行参数覆盖机制
- 构建系统通过Makefile将 `json_config.cpp` 纳入编译流程（第127行）