---
kind: logging_system
name: 日志系统 — 基于 printf/标准流与 StatsCollector 的混合输出
category: logging_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/stats_collector.h
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/cache_src/common/types.h
    - main.cpp
---

本仓库未引入第三方日志框架，日志输出采用两种并行的轻量方式：

1. **调试/追踪日志**：在各模块源码中直接使用 `printf` / `fprintf` / `std::cout` / `std::cerr` 输出带前缀标签的行级日志（如 `[DDR]`、`[MSI_TRANSLATION]`、`[PT_CACHE]`、`[WALKER_CACHE]`、`[DEDUP_CACHE]` 等），并通过 `fflush(stdout)` 强制刷新。这类日志主要用于功能调试与路径追踪，无统一级别控制或结构化字段。

2. **统计与报告日志**：通过 `iommu/cache_src/common/stats_collector.h/.cpp` 中的 `StatsCollector` 类集中收集缓存命中率、延迟直方图、RAM 访问路径计数、阶段时间戳等指标，并以固定格式写入统一文件。该文件路径由 `StatsConfig::output_file`（默认 `cache_sim.log`）配置，可通过 JSON 配置项 `unified_log_file` 覆盖。

关键约定与约束：
- 所有运行期文本输出均直接写向 stdout/stderr 或 `std::ofstream`，没有统一的 log level 枚举或开关宏。
- 统计输出通过 `StatsCollector::set_output_file()` 打开文件，`write_log()` 逐行追加并 flush；`report()` 一次性输出 summary + histogram 并 flush。
- 任务 trace 开关由 `StatsConfig::enable_task_trace` 与 `task_trace_level`（off/basic/detail）控制，但当前代码中未见启用 trace 的实际调用点。
- 错误提示使用 `std::cerr` 输出警告（如无法打开输出文件时）。