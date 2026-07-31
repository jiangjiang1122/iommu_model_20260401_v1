---
kind: logging_system
name: 日志系统 — 基于 printf/iostream 的分散式调试输出与 StatsCollector 统计报告
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/cache_src/common/json_config.cpp
    - iommu/include/iommu_task.hh
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - ddr/test_ddr.cc
---

本仓库未采用统一的日志框架（如 spdlog、glog 等），而是以两种互补的方式组织输出：

1) 代码级调试输出（printf / std::cout / std::cerr）
- 各模块在关键路径上直接调用 printf 或 std::cout 打印带前缀标签的行，例如 `[DDR]`、`[MSI_TRANSLATION]`、`[PT_CACHE]`、`[WALKER_CACHE]`、`[DEDUP_BUFFER]`、`[RP_RSP]` 等。
- 这些输出主要用于开发期定位问题，没有统一封装，也没有集中开关；部分模块通过 `#ifdef DEBUG_*`（如 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS`）控制是否输出，由顶层 `main.cpp` 中的 `#ifdef DEBUG` 宏批量开启。
- 错误信息通常走 `std::cerr`，例如 `StatsCollector::set_output_file` 打开文件失败时打印 Warning。

2) 结构化统计报告（StatsCollector）
- 位于 `iommu/cache_src/common/stats_collector.cpp`，提供对每个 Cache 子系统的访问/命中/缺失/预取/淘汰/失效计数、执行延迟直方图、队列延迟直方图、请求延迟统计、按阶段（REQUEST/UPDATE/MONITOR）细分统计、区间命中率等。
- 通过 `print_summary(std::ostream&)` 将汇总表格输出到任意流（默认 stdout），并通过 `set_output_file(path)` + `report()` 把完整报告写入文件，支持可选的 `enable_latency_histogram` 和 `task_trace_level` 配置项。
- 配置文件通过 `json_config.cpp` 解析 `statistics.output_file` / `unified_log_file` / `enable_latency_histogram` / `task_trace_level` / `histogram_bin_width_ns` 等字段，其中 `unified_log_file` 作为 `output_file` 的别名回退。
- 缓存子系统还通过 `level == "detail"|"detailed"|"debug"|"verbose"` 字符串判断是否启用详细日志（见 `cache_subsystem.cpp`）。

3) 任务级 DDR 访问追踪字段
- `iommu/include/iommu_task.hh` 中为每个任务维护 `ddr_log_type[8]`、`ddr_log_level[8]`、`ddr_log_addr[8]`、`ddr_log_size[8]`、`ddr_log_count` 数组，配合 `iommu_perf_model/iommu_perf_ptw.cc` 在页表遍历过程中记录每次 DDR 访问的类型、页表层级、地址和大小，用于事后分析 PTW 行为。

约定与约束
- 调试输出使用固定前缀标签（如 `[MODULE_NAME]`）便于 grep 过滤，但未形成统一的 log macro。
- 性能统计必须通过 `StatsCollector` 接口记录，禁止在各模块内自行维护统计变量。
- 输出目标可通过 JSON 配置切换至文件，但运行时没有动态日志级别开关，需重新编译或依赖外部脚本过滤输出。
- 所有时间戳基于 SystemC 仿真时间 `sc_core::sc_time_stamp()`，单位纳秒。