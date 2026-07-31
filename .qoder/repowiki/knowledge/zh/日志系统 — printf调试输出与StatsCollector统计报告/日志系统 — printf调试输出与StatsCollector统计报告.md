---
kind: logging_system
name: 日志系统 — printf调试输出与StatsCollector统计报告
category: logging_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/stats_collector.h
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/include/iommu_task.hh
    - main.cpp
---

本仓库未采用统一的日志框架，而是以两种互补的方式输出运行时信息：

1. **printf调试日志**（开发/调试阶段）
- 各模块通过 `printf` / `snprintf` 直接输出结构化文本行，统一以 `[MODULE]` 前缀区分来源，如 `[WALKER_CACHE]`、`[PT_CACHE]`、`[DEDUP_CACHE]`、`[MSI_TRANSLATION]`、`[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[RP_RSP]`。
- 关键路径（Walker Cache HIT/MISS/UPDATE、PT Cache Multi-RAM配置、MSI地址翻译步骤、DDR请求/响应）均包含时间戳 `t=%llu ns` 和关键字段（gscid、pscid、iova/gpa、level等），便于逐条回放分析。
- 错误路径使用 `ERROR:` 或 `Warning:` 前缀，并通过 `std::cerr` 输出到标准错误流。
- 这些输出由编译期宏控制（见 `main.cpp` 中的 `DEBUG_*` 宏定义链），但代码中并未对每个 `printf` 加 `#ifdef DEBUG` 保护，属于“默认开启”的调试输出风格。

2. **StatsCollector统计报告**（性能度量）
- 位于 `iommu/cache_src/common/stats_collector.h/.cpp`，提供 `StatsCollector` 类，按缓存名称聚合 `CacheStats`（命中率、缺失率、预取准确率、执行/排队/请求延迟、IOPS、RAM访问路径计数、阶段分类计数、区间命中率、直方图等）。
- 通过 `set_output_file()` 设置输出文件路径，由 `json_config.cpp` 从JSON配置的 `output_file` / `unified_log_file` 字段注入；`cache_subsystem.cpp` 在初始化时调用 `stats_.set_output_file(cfg.statistics.output_file)`。
- 提供 `write_log(msg)` 写入任意文本行，`report()` 将 summary + histogram 统一写入输出文件；失败时通过 `std::cerr` 打印警告。
- 该组件被 `Makefile` 和 `build_cpp.sh` 显式纳入构建，是仓库中唯一被集中管理的“结构化输出”子系统。

3. **任务级DDR日志字段**（数据内嵌）
- `iommu/include/iommu_task.hh` 在 `walk_context_t` 中定义了 `ddr_log_type[]`、`ddr_log_level[]`、`ddr_log_addr[]`、`ddr_log_size[]`、`ddr_log_count` 等数组字段，用于记录每次DDR访问的类型、页表层级、地址和大小，作为任务上下文的一部分随流水线传递，供后续分析脚本提取。

**架构与约定**
- 无全局logger单例，各模块自行 `printf` 输出，格式约定靠注释和前缀保持一致。
- 性能统计通过 `StatsCollector` 集中收集，但调用点分散在各缓存实现中（通过 `record_*` API 注册事件）。
- 输出目标：调试日志走 stdout/stderr，统计报告走可配置的文件路径（JSON配置驱动）。
- 没有统一的log level机制（INFO/WARN/ERROR分级），仅通过前缀和 `ERROR:` / `Warning:` 文本区分严重性。

**约束与限制**
- `StatsCollector::report()` 依赖 `set_output_file()` 先打开文件，否则 `std::cerr` 报错（见 stats_collector.cpp L547/L560）。
- JSON配置中 `output_file` 与 `unified_log_file` 两个字段均可设置统计输出路径，后者会覆盖前者（见 json_config.cpp L134-L137）。
- 所有 `printf` 输出未做缓冲控制，仿真中频繁调用可能影响性能，属于纯调试用途。