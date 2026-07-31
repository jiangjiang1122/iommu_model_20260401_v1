---
kind: logging_system
name: 基于printf与StatsCollector的混合日志与统计机制
category: logging_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/stats_collector.h
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/iommu_top.cc
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - main.cpp
---

该 RISC-V IOMMU SystemC 仿真模型未采用专用的第三方日志框架（如 spdlog 或 glog），而是采用了一种**“控制台调试打印 + 结构化统计收集器”**的混合日志体系。

### 1. 核心日志策略
*   **控制台输出 (Console Logging)**：主要依赖 C 标准库的 `printf`/`fflush(stdout)` 进行实时状态追踪。由于 SystemC 仿真涉及并发进程，代码中广泛使用 `fflush(stdout)` 确保日志在仿真时间推进前即时刷新，避免缓冲导致的时序错乱。
*   **结构化统计 (Structured Statistics)**：通过自定义的 `iommu::StatsCollector` 类收集缓存命中率、延迟直方图、IOPS 等性能指标，并在仿真结束时生成详细报告。
*   **SystemC 原生报告**：仅在严重错误（如非法 FIFO 操作）时使用 `SC_REPORT_ERROR`。

### 2. 关键组件与文件
*   **`iommu/cache_src/common/stats_collector.{h,cpp}`**：核心统计引擎。支持注册多个缓存通道（dc_cache, pt_cache 等），记录命中/缺失、预取准确率、执行/排队延迟，并支持将报告写入 `cache_sim.log`。
*   **`iommu/iommu_top.cc`**：顶层模块负责汇总全局统计。在 `print_cache_statistics()` 中调用 `StatsCollector::print_summary`，并补充打印 VA Dedup、Walker Cache 更新分布及端口带宽利用率。
*   **`iommu/cache_src/subsystem/cache_subsystem.cpp`**：缓存子系统内部实现了详细的任务级追踪（Task Trace）。通过 `trace_task_event` 方法，根据配置将 `[cache-task]` 格式的结构化日志写入输出文件，用于离线分析流水线行为。

### 3. 日志分类与约定
*   **调试标签 (Debug Tags)**：代码中使用方括号标签区分不同模块，例如 `[IOMMU_TOP]`, `[DDR_ARBITER]`, `[PT_CACHE_EXECUTE]`, `[VA_DEDUP_RECOVER]`。这种模式便于使用 `grep` 过滤特定子系统的行为。
*   **性能埋点**：在关键路径（如 FIFO 读写、DDR 仲裁、Cache 查找）嵌入时间戳计算逻辑，自动累加延迟样本。
*   **条件编译**：`main.cpp` 中定义了 `DEBUG_TRANSLATION`, `DEBUG_TWOSTAGE` 等宏，用于控制特定功能块的详细打印输出。

### 4. 开发者指南
*   **添加新日志**：对于运行时调试，直接使用 `printf("[MODULE_NAME] message...\n"); fflush(stdout);`。
*   **性能监控**：若需监控新模块的性能，应在 `StatsCollector` 中注册新通道，并使用 `record_access`, `record_hit`, `record_latency` 等方法埋点。
*   **日志持久化**：仿真产生的详细统计和任务追踪日志默认输出至 `cache_sim.log`，可通过 `GlobalConfig` 修改输出路径。