---
kind: configuration_system
name: IOMMU 缓存子系统 JSON 配置系统
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

该仓库为 IOMMU 地址翻译缓存子系统与去重预取架构的 SystemC 模型，其配置系统采用轻量级 JSON 文件驱动，通过自实现的 mini_json 解析器加载并分层合并默认值与用户覆盖。

系统与工具
- 使用自实现的 mini_json（iommu/cache_src/common/mini_json.h）作为零依赖 JSON 解析器，支持对象、数组、字符串、布尔、数字、null 以及 // 行注释和 /* */ 块注释。
- 配置入口为 json_config.cpp 中的 load_config(json_path) 与 parse_config(json_str)，返回统一的 GlobalConfig 结构体。

核心数据结构
- CacheConfig：描述单个 cache 的几何（num_sets、num_ways）、替换策略（replacement: plru/srrip/none）、SR-RIP m 位、多 RAM 分组（num_rams、ram_fifo_depth）及各级延迟参数。
- StatsConfig：统计输出文件、延迟直方图开关、task trace 级别等。
- GlobalConfig：聚合 global（clock_period_ns、random_seed）、各 cache（dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache）、walker_cache（base_sets + ptw_c1/c2/c3）与 statistics。

配置文件组织与约定
- 默认配置位于 iommu/cache_config/default_config.json，示例覆盖配置位于 iommu/cache_config/input_params_example.json。
- JSON 顶层键包括 global、cache_timing、dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache、statistics。
- 每个 cache 段可省略部分字段；未提供的字段会继承 cache_timing 公共默认或该 cache 类型的硬编码默认（如 dc/pc/msipt 默认 64×4 plru，pt 默认 1024×8 srrip m_bits=2）。
- dedup_cache 若未显式配置则复用 pt_cache 的 num_sets/num_ways，并强制 num_rams=4、ram_fifo_depth=8。
- walker_cache 通过 base_sets 统一三级的 sets，再分别指定 ptw_c1/c2/c3 的 ways 与 replacement。
- statistics.output_file 与 statistics.unified_log_file 存在兼容逻辑：若同时提供，优先使用 output_file。

加载流程与默认值合并
- load_config(path) 打开文件读取文本后调用 parse_config(str)。
- parse_config 按段解析，对每个 cache 先构造带硬编码默认值的 CacheConfig，再用 JSON 中对应段逐项覆盖（仅当 key 存在时赋值），最后将结果写入 GlobalConfig 对应成员。
- 缺失文件会抛出 std::runtime_error("Cannot open config file: ...")。

约束与行为
- JSON 键名必须与代码中 contains(...) 检查一致，新增字段需同时在解析函数中添加处理分支。
- 类型转换通过 mini_json::Value 的 operator 完成，类型不匹配会抛出运行时异常。
- 所有延迟参数均为 cycle 数，由上层性能模型根据 clock_period_ns 换算为时间。
- 配置系统不校验字段取值范围（如 num_rams 需为 2 的幂且整除 num_sets 的约束由使用者保证）。