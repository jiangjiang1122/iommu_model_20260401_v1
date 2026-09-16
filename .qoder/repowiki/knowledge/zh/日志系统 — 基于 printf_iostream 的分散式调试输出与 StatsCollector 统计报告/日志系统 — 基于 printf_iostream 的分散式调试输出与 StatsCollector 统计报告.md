---
kind: logging_system
name: 日志系统 — 基于 printf/iostream 的分散式调试输出与 StatsCollector 统计报告
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/iommu_top.cc
    - iommu/cache_src/cache/walker_cache.cpp
    - iommu/cache_src/cache/pt_cache.cpp
    - iommu/cache_src/cache/dedup_cache.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/cache_src/common/stats_collector.cpp
    - rp/test_rp.hh
---

本仓库未采用统一的日志框架（如 spdlog、glog、SystemC sc_log 等），而是以**分散式 printf/iostream 输出**作为主要调试手段，并辅以 `StatsCollector` 模块提供结构化的性能统计报告。整体属于“无专用日志系统”的实现方式。

### 1. 使用的系统与工具
- **printf / fprintf**：绝大多数模块使用 `printf("[TAG] ...")` 形式输出调试信息，标签统一用方括号前缀（如 `[IOMMU_TOP]`、`[DDR_ARBITER]`、`[PT_CACHE]`、`[WALKER_CACHE]`、`[MSI_TRANSLATION]`、`[VA_DEDUP_RECOVER]` 等）。
- **std::cout / std::cerr**：仅在统计模块 `stats_collector.cpp` 中使用 `std::cerr` 打印文件打开失败的警告，以及通过 `std::cout` 输出汇总报告。
- **fflush(stdout)**：在关键路径（任务入口、响应返回、仲裁路由等）后调用 `fflush(stdout)` 保证实时可见性。
- **StatsCollector**：位于 `iommu/cache_src/common/stats_collector.{cpp,hh}`，负责将命中率、延迟直方图、队列延迟等指标写入文件，是仓库中唯一具备“结构化输出能力”的组件。

### 2. 核心文件与位置
- `main.cpp`：仿真入口，仅用 `printf` 打印 SystemC 版本和启动/结束信息。
- `iommu/iommu_top.cc`：顶层模块，包含大量 `printf` 调试输出（任务路由、DDR仲裁、响应发送、Walker前置查询、VA去重恢复、Cache统计打印等）。
- `iommu/cache_src/cache/*.cpp`：各 Cache 实现（pt_cache、walker_cache、dedup_cache）内部使用带时间戳 `[t=%llu ns][CACHE_TAG]` 格式的 printf 输出 HIT/MISS/UPDATE/LAZY_INVAL 等事件。
- `iommu/iommu_fun_model/iommu_msi_trans.cc`：MSI 翻译流程逐步骤 `printf` 调试。
- `iommu/cache_src/common/stats_collector.cpp`：唯一集中式统计输出，提供 `set_output_file()`、`write_log()`、`report()`、`print_summary()` 等接口。
- `rp/test_rp.hh`：测试宏 `fail_if`、`END_TEST` 使用 `printf` 输出测试结果。

### 3. 架构与约定
- **无全局日志级别控制**：没有统一的 log level 开关，所有 `printf` 直接输出，无法按级别过滤。
- **标签化前缀约定**：每个模块/子系统定义自己的 TAG（如 `[IOMMU_TOP]`、`[DDR_ARBITER]`、`[PT_CACHE_LAZY_INVAL]`、`[WALKER_S2_CACHE]`），便于 grep 过滤。
- **时间戳格式**：部分关键路径（Walker Cache、PTW 相关）在消息开头附加 `[t=%llu ns]` 纳秒时间戳，用于时序分析。
- **错误输出通道**：`std::cerr` 仅用于 `StatsCollector` 的文件打开失败警告；业务错误通过 `printf` + ANSI 颜色码（如 `\x1B[31mFAIL...\x1B[0m`）输出到 stdout。
- **统计与日志分离**：性能统计通过 `cache_sub.stats().print_summary()` 或 `StatsCollector::report()` 输出到文件或 `std::cout`，与调试 `printf` 完全解耦。

### 4. 约定与约束
- **无强制规范**：代码中未发现对日志格式、级别、输出的强制性约束或文档规定，所有输出均为开发期调试用途。
- **可观测性依赖 grep**：由于缺乏结构化日志框架，实际调试依赖对标准输出的 grep 过滤（如 `grep "\[WALKER_CACHE\]"`）。
- **性能影响**：高频路径（Walker Cache HIT/MISS、DDR仲裁）存在大量 `printf` + `fflush`，可能显著影响仿真性能，但未见条件编译开关（如 `#ifdef DEBUG_LOG`）进行控制。
- **StatsCollector 是唯一受控输出**：仅该模块提供文件输出能力和直方图统计，其他模块均直接向 stdout 输出。

### 结论
该仓库**不存在专门的日志系统**，调试输出完全依赖分散的 `printf`/`iostream` 调用，仅 `StatsCollector` 提供了有限的结构化统计报告能力。若需引入统一日志系统，建议在此基础上抽象出统一的 logger 接口，并增加日志级别控制和条件编译开关。