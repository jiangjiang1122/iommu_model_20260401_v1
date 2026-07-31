---
kind: configuration_system
name: 基于JSON的缓存与统计配置系统
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

该仿真模型采用轻量级JSON配置文件驱动的配置系统，专门用于控制多RAM去重Cache、PT Cache、Walker Cache等子系统的几何参数、时序延迟和统计输出。核心实现位于 `iommu/cache_src/common/` 目录下，通过自研的 `mini_json.h` 解析器加载配置，避免引入外部依赖。

**架构设计**：
- 配置结构体分层定义：`GlobalConfig`（全局时钟周期、随机种子）→ `CacheConfig`（sets/ways/replacement/时序延迟）→ `StatsConfig`（统计输出开关）
- 支持默认值继承机制：每个cache类型都有预设默认值（如dc_cache=64x4 plru，pt_cache=1024x8 srrip），JSON中仅覆盖需要修改的字段
- 提供两种加载方式：`load_config(json_path)`从文件加载，`parse_config(json_str)`从字符串解析

**配置层次**：
1. `global`：clock_period_ns、random_seed
2. `cache_timing`：仲裁器、hash、read_set、compare等通用时序延迟
3. 各cache独立配置：dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache
4. walker_cache：base_sets + ptw_c1/c2/c3三级缓存配置
5. statistics：输出文件路径、直方图、任务追踪开关

**约束与验证**：
- JSON字段使用`contains()`检查可选性，未指定字段保持默认值
- 文件打开失败抛出`std::runtime_error`异常
- dedup_cache默认复用pt_cache的几何参数并设置num_rams=4、ram_fifo_depth=8
- walker_cache各层级强制覆盖base_sets参数确保一致性

**配置文件示例**：`default_config.json`提供完整默认配置，`input_params_example.json`展示精简配置写法。当前main.cpp尚未集成此配置系统，需通过命令行参数或环境变量传入配置文件路径后调用`load_config()`初始化。