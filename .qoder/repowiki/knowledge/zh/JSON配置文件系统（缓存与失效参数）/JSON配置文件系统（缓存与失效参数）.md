---
kind: configuration_system
name: JSON配置文件系统（缓存与失效参数）
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
---

该仓库为基于SystemC的RISC-V IOMMU性能模型，其配置系统围绕**轻量级JSON文件加载**实现，用于驱动多Cache（DC/PC/PT/Walker/MSIPT/Dedup）几何、时序、失效策略及统计输出等运行时参数。

### 1. 使用的系统与工具
- **自研mini_json解析器**：`iommu/cache_src/common/mini_json.h` 提供无第三方依赖的JSON Value/Parser，支持对象、数组、布尔、整型、浮点、字符串及 `//` / `/* */` 注释。
- **json_config模块**：`iommu/cache_src/common/json_config.{h,cpp}` 暴露 `load_config(json_path)` 与 `parse_config(json_str)` 两个入口，将JSON映射到 `GlobalConfig` 结构体。
- **配置数据结构**：`iommu/cache_src/common/types.h` 中定义 `CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig` 等结构体，作为配置的强类型载体。

### 2. 核心文件与包
- `iommu/cache_src/common/mini_json.h` — 最小JSON解析器
- `iommu/cache_src/common/json_config.h/.cpp` — JSON→GlobalConfig 解析逻辑
- `iommu/cache_src/common/types.h` — 所有配置结构体定义（`CacheConfig`、`GlobalConfig` 等）
- `iommu/cache_config/default_config.json` — 完整默认配置模板
- `iommu/cache_config/input_params_example.json` — 精简示例配置
- `iommu/cache_src/subsystem/cache_subsystem.h` — 通过 `CacheSubsystem(sc_module_name, const GlobalConfig&)` 消费配置

### 3. 架构与约定
- **分层配置结构**：`GlobalConfig` 聚合全局时钟/随机种子、各Cache实例的 `CacheConfig`、Walker三级子缓存、失效配置与统计配置；每个 `CacheConfig` 包含 sets/ways/replacement/srrip_m_bits/ram分组与时序延迟字段。
- **默认值+覆盖模式**：解析时先填充硬编码默认值（如 dc_cache=64x4 plru、pt_cache=1024x8 srrip），再按JSON中存在的键逐项覆盖，未出现的字段保持默认。
- **可选字段安全访问**：所有JSON键读取均使用 `j.contains("key")` 判断后再赋值，缺失字段不会抛异常。
- **统一消息结构承载配置**：`CacheMessage` 在 types.h 中定义了 lookup/update/invalidate 的统一载荷，使配置驱动的缓存行为与消息流解耦。
- **失效子系统独立配置**：`invalidation` 节点控制懒失效(LIB/VN)开关、LIB大小、VN位宽与匹配周期。

### 4. 约定与约束
- **JSON键命名规范**：所有配置键使用小写蛇形命名（如 `num_sets`、`replacement`、`lazy_enable`），与C++结构体字段一一对应。
- **必需字段**：`global.clock_period_ns` 和 `global.random_seed` 是全局必需项；各cache节点可省略，省略则使用默认几何。
- **替换策略枚举**：`replacement` 仅接受 `"plru"`、`"srrip"`、`"none"` 三种字符串，由对应策略类实现。
- **哈希模式**：`hash_mode` 仅支持 `"inval_v2"`（新哈希，支持addr枚举失效）与 `"legacy"`（旧哈希，回归对比）。
- **RAM分组约束**：`num_rams` 必须为2的幂且能整除 `num_sets`，否则运行时行为未定义。
- **统计输出**：`statistics.output_file` 可通过 `unified_log_file` 别名设置，二者冲突时优先 `output_file`。
- **错误处理**：`load_config` 打开文件失败时抛出 `std::runtime_error`；JSON解析非法时由 mini_json 抛出异常。