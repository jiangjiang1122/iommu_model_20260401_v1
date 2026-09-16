---
kind: configuration_system
name: 基于JSON的IOMMU缓存子系统配置系统
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

## 1. 系统与工具

该仓库采用**自实现的轻量级 JSON 配置加载机制**，用于驱动 RISC-V IOMMU SystemC 性能模型中的缓存子系统（DC/PC/PT/MSIPT/Walker/Dedup Cache）。核心组件为 `iommu/cache_src/common/json_config.{h,cpp}`，依赖同目录下的 `mini_json.h` 完成 JSON 解析；没有使用外部 JSON 库或环境变量注入。

## 2. 关键文件与包

- **类型定义**：`iommu/cache_src/common/types.h` — 定义 `CacheConfig`、`InvalidationConfig`、`StatsConfig`、`GlobalConfig` 等配置结构体，作为 JSON 字段到 C++ 结构的映射目标。
- **解析器接口**：`iommu/cache_src/common/json_config.h` — 暴露 `load_config(const std::string& json_path)` 和 `parse_config(const std::string& json_str)` 两个入口。
- **解析实现**：`iommu/cache_src/common/json_config.cpp` — 通过 `mini_json::Parser::parse` 解析 JSON，按段填充 `GlobalConfig`；每个子模块（dc_cache / pc_cache / msipt_cache / pt_cache / dedup_cache / walker_cache / invalidation / statistics）都有独立的 `if (j.contains("..."))` 分支，缺失字段则保留默认值。
- **默认配置文件**：`iommu/cache_config/default_config.json` — 完整示例，包含所有可选段。
- **简化示例**：`iommu/cache_config/input_params_example.json` — 仅含必要字段的精简版。
- **顶层装配**：`iommu/iommu_top.hh` 中 `iommu_top` 成员在构造时调用 `iommu::load_config("iommu/cache_config/default_config.json")`，将结果传入 `CacheSubsystem` 构造函数。
- **Mini JSON 解析器**：`iommu/cache_src/common/mini_json.h`（被 json_config.cpp include），提供 `Value`、`Parser::parse` 等基础 API。

## 3. 架构与设计约定

### 3.1 分层配置结构
`GlobalConfig` 是根节点，包含：
- `global`：`clock_period_ns`、`random_seed`（全局仿真参数）
- `cache_timing`：共享给各 cache 的时序延迟（arbiter/hash/read_set/compare/update_way_select/fill_compute_index*/write_way/invalidation_compare_per_way cycles）
- 各 cache 独立段：`dc_cache`、`pc_cache`、`msipt_cache`、`pt_cache`、`dedup_cache`、`walker_cache.ptw_c1/c2/c3`
- `invalidation`：失效总开关、懒失效(LIB)开关、LIB大小、VN位宽、匹配周期
- `statistics`：输出文件、延迟直方图开关、task trace 级别、直方图桶宽

### 3.2 默认值继承策略
每个 cache 段在解析前先构造一个“默认 `CacheConfig`”，再合并 JSON 覆盖：
- `dc_cache` / `pc_cache` / `msipt_cache` 默认 `num_sets=64, num_ways=4, replacement="plru"`
- `pt_cache` 默认 `num_sets=1024, num_ways=8, replacement="srrip", srrip_m_bits=2`
- `dedup_cache` 几何复用 `pt_cache`，并固定 `num_rams=4, ram_fifo_depth=8`
- `walker_cache` 的 c1/c2/c3 分别默认 `num_ways=1/2/4`，replacement 分别为 `none/plru/plru`
- `cache_timing` 可被各 cache 单独覆盖（如 `default_config.json` 中 dc/pc 的 `read_set_latency_cycles=1` 覆盖全局的 2）

### 3.3 可选字段语义
解析器对每个 JSON 字段使用 `j.contains("...")` 判断是否存在，**不存在即跳过**，不会报错。这使得用户只需提供差异化的增量配置即可。

### 3.4 统计输出的向后兼容
`statistics.output_file` 与 `statistics.unified_log_file` 二选一：若同时存在，优先取 `output_file`；否则回退到 `unified_log_file`。

## 4. 约定与约束

- **配置文件路径硬编码**：`iommu_top.hh` 第 60 行直接写死 `"iommu/cache_config/default_config.json"`，未从命令行参数或环境变量读取。修改配置需改源码或替换该路径字符串。
- **JSON 键名必须精确匹配**：字段名区分大小写且与 `json_config.cpp` 中 `contains` 检查一致（如 `num_sets`、`num_ways`、`replacement`、`srrip_m_bits`、`enable`、`lazy_enable` 等），拼写错误会被静默忽略。
- **数值类型强制转换**：所有 JSON 数值经 `static_cast<uint32_t>` / `static_cast<double>` 转换，无范围校验；溢出行为取决于底层 mini_json 实现。
- **缺失段不报错**：缺少整个段（如没有 `walker_cache`）时对应字段保持 `types.h` 中的默认初始化值。
- **运行时不可变**：配置在 `sc_main` 启动前一次性加载进 `iommu_top::cfg`，后续无法动态修改。
- **无环境变量/命令行覆盖层**：当前实现未集成 `argc/argv` 解析或 `getenv`，所有运行期参数均来自 JSON 文件。
- **构建产物可见性**：`build_log.txt` 显示 `json_config.o` 参与链接，确认该模块已纳入编译流程。

## 5. 总结

该配置系统是一个**轻量级、JSON 驱动的静态配置方案**，专为 IOMMU 缓存子系统（DC/PC/PT/MSIPT/Walker/Dedup）设计，通过 `GlobalConfig` 聚合多类参数，并以“默认值 + 可选覆盖”的方式支持灵活调参。其优点是实现简单、无需外部依赖；缺点是缺乏运行时热更新、环境变量覆盖和严格的 schema 校验，适合离线仿真场景的参数探索。