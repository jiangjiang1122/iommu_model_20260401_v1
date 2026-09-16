---
kind: configuration_system
name: 基于JSON的IOMMU缓存与子系统配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/iommu_top.hh
---

## 1. 使用的系统与工具

本项目采用**自实现的轻量级 JSON 配置加载机制**，不依赖第三方配置库。核心由 `iommu/cache_src/common/json_config.{h,cpp}` 提供，底层使用同目录下的 `mini_json.h` 进行 JSON 解析。配置以纯文本 JSON 文件形式存放于 `iommu/cache_config/` 目录下，通过 `load_config()` / `parse_config()` 两个入口函数加载到 C++ 结构体中。

## 2. 关键文件与包

- **配置解析层**
  - `iommu/cache_src/common/json_config.h`：声明 `GlobalConfig load_config(const std::string&)` 与 `GlobalConfig parse_config(const std::string&)`。
  - `iommu/cache_src/common/json_config.cpp`：实现 JSON→结构体的映射，包含 `parse_cache_timing_config()`、`parse_cache_config()` 等内部解析器。
  - `iommu/cache_src/common/mini_json.h`：极简 JSON 解析器（`mini_json::Parser::parse`）。
  - `iommu/cache_src/common/types.h`：定义所有配置结构体 `CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig`。

- **配置文件**
  - `iommu/cache_config/default_config.json`：默认完整配置，覆盖 global、cache_timing、dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache、statistics、invalidation 全部段。
  - `iommu/cache_config/input_params_example.json`：精简示例，仅含必要字段，用于演示最小可运行配置。

- **顶层集成点**
  - `iommu/iommu_top.hh`：在模块成员初始化处调用 `iommu::load_config("iommu/cache_config/default_config.json")`，将结果构造 `CacheSubsystem`。
  - `main.cpp`：创建 `iommu_top` 实例，间接触发配置加载。

## 3. 架构与设计约定

### 3.1 配置结构层次
`GlobalConfig` 是根节点，聚合以下子配置：
- `global`：`clock_period_ns`（仿真时钟周期）、`random_seed`（随机种子）。
- `cache_timing`：全局共享的时序延迟参数（arbiter/hash/read_set/compare/update_way_select/fill_compute_index_*_cycles/write_way_latency_cycles/invalidation_compare_per_way_cycles），被各 cache 作为默认值继承。
- 各 Cache 独立配置：`dc_cache`、`pc_cache`、`msipt_cache`、`pt_cache`、`dedup_cache`，均继承 `cache_timing` 默认值后按自身语义覆盖（如 PT Cache 默认 1024×8、srrip 替换；DC/PC/MSIPT 默认 64×4、plru）。
- `walker_cache.base_sets` + `walker_cache.ptw_c1/c2/c3`：Walker 三级缓存共用 base_sets，每级独立 ways/replacement/ram 参数。
- `invalidation`：失效总开关、懒失效开关、LIB 容量、VN 位宽、匹配延迟。
- `statistics`：输出文件、直方图开关、task trace 级别、bin 宽度。

### 3.2 默认值与合并策略
每个 cache 段在解析前先构造一个带“硬编码默认值”的 `CacheConfig`（例如 DC/PC/MSIPT 默认 64 sets × 4 ways × plru；PT 默认 1024×8×srrip；dedup 复用 pt_cache 几何并固定 num_rams=4、ram_fifo_depth=8），再对 JSON 中存在的字段逐项覆盖。缺失字段不会报错，而是保持默认值——这是一种**宽松合并**策略。

### 3.3 加载流程
1. `iommu_top` 成员初始化阶段调用 `load_config("iommu/cache_config/default_config.json")`。
2. `load_config` 打开文件读取全部内容字符串，委托给 `parse_config`。
3. `parse_config` 用 `mini_json::Parser::parse` 解析为 `Value` 树，逐段检查 `contains(...)` 后填充 `GlobalConfig`。
4. 若文件无法打开，抛出 `std::runtime_error`。

### 3.4 运行时扩展点
- 统计输出路径可通过 `statistics.output_file` 或兼容字段 `statistics.unified_log_file` 指定。
- Walker Cache 的 `base_sets` 同时决定 c1/c2/c3 的 set 数，形成“基线+分级”的配置模式。
- 去重缓存（dedup_cache）默认几何与 PT Cache 保持一致，但强制 `num_rams=4, ram_fifo_depth=8`，体现硬件约束。

## 4. 约定与约束

- **配置格式**：必须为合法 JSON，键名严格区分大小写（如 `cache_timing`、`invalidation`、`statistics`）。
- **可选字段**：所有 JSON 字段均为可选，缺失时回退到代码内硬编码默认值；新增字段需先在 `json_config.cpp` 中添加 `if (j.contains(...))` 分支。
- **类型安全**：解析时使用 `static_cast<uint32_t>` / `static_cast<double>` / `static_cast<bool>` 显式转换，未做范围校验。
- **单配置源**：当前 `iommu_top` 硬编码加载 `iommu/cache_config/default_config.json`，未在命令行或环境变量中提供覆盖机制。
- **向后兼容**：`statistics.output_file` 与 `statistics.unified_log_file` 两个键均可设置同一目标，后者作为兼容别名。
- **失效配置耦合**：`invalidation.enable` 控制整个失效通路开关；`lazy_enable` 控制是否走 LIB 延迟失效，关闭时降级为立即扫表。
- **多 RAM 约束**：`num_rams` 需为 2 的幂且整除 `num_sets`（见 `types.h` 注释），该约束由使用者保证，解析器不做校验。
- **哈希模式**：`hash_mode` 支持 `inval_v2`（新哈希，支持 addr 枚举失效）和 `legacy`（旧哈希，仅回归对比）两种模式。

## 5. 与其他模块的关系

- `CacheSubsystem`（`cache_subsystem.{h,cpp}`）接收 `GlobalConfig` 并据此实例化 DC/PC/PT/MSIPT/Walker/Dedup 各级缓存。
- 性能模型侧（`iommu_perf_model/`）另有独立的 perf 参数解析（`iommu_perf_parser.cc`、`iommu_perf_params*.hh`），与缓存配置系统分离，分别服务于功能仿真与性能建模两条路径。
- 测试驱动（`rp/test_rp*.cc`）通过构造不同场景的 IOMMU 行为来验证配置生效后的缓存命中/失效/预取效果，而非直接修改配置。
