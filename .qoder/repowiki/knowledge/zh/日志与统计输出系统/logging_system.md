该 RISC-V IOMMU SystemC 性能模型未采用专用的第三方日志框架（如 spdlog、glog），而是基于 C++ 标准库和 SystemC 原生机制构建了一套混合式的日志与统计输出系统。其核心目标是支持高性能仿真过程中的状态追踪、缓存行为分析以及最终的性能报告生成。

### 1. 核心组件与架构

*   **结构化统计收集器 (`StatsCollector`)**:
    *   位于 `iommu/cache_src/common/stats_collector.{h,cpp}`，是系统中最接近“日志系统”的核心模块。
    *   **功能**：负责收集各缓存（DC, PC, PT, Walker, MSIPT）的命中率、延迟直方图、预取准确率等关键性能指标。
    *   **输出机制**：通过 `set_output_file` 配置输出路径（默认为 `cache_sim.log`），利用 `std::ofstream` 将格式化的统计摘要和直方图数据写入文件。`write_log` 方法支持向该文件追加自定义的结构化日志消息。
    *   **任务级追踪**：支持 `TaskTraceLevel`（OFF/BASIC/DETAIL），在 `CacheSubsystem` 中通过 `trace_task_event` 生成包含时间戳、任务ID、操作类型和命中状态的详细追踪日志。

*   **控制台即时调试 (`printf/std::cout`)**:
    *   广泛分布于 `iommu/iommu_perf_model` 和 `iommu/cache_src` 目录下。
    *   **用途**：用于仿真运行时的关键路径调试和状态确认（如 `[COLLECTOR]`, `[PT_CACHE_EXECUTE]`, `[DC Lookup]` 等前缀）。
    *   **特点**：大量使用 `fflush(stdout)` 确保在多线程/多进程 SystemC 仿真环境下的输出实时性，防止因缓冲区导致的日志乱序或丢失。

*   **SystemC 原生报告机制**:
    *   仅在极少数严重错误场景下使用 `SC_REPORT_ERROR`（如 `cache_subsystem.cpp` 中的 FIFO 非法访问检查）。
    *   未观察到对 `sc_report_handler` 的自定义配置，表明系统主要依赖标准输出和文件流进行信息记录。

### 2. 日志 conventions 与规则

*   **结构化字段**：在 `StatsCollector` 生成的日志中，采用键值对形式（如 `task_id=`, `hit=`, `latency=`）记录事件，便于后续通过 Python 脚本（如 `tmp/analyze_*.py`）进行自动化分析。
*   **分级策略**：
    *   **L0 (Console)**: 关键状态变更、错误报警、仿真进度（使用 `printf`）。
    *   **L1 (File - Summary)**: 仿真结束后的全局统计摘要（由 `StatsCollector::report()` 触发）。
    *   **L2 (File - Trace)**: 细粒度的任务生命周期追踪（由 `trace_task_event` 根据配置级别触发）。
*   **同步要求**：由于 SystemC 仿真的并发特性，所有控制台输出在关键逻辑后均强制刷新缓冲区，以确保日志与仿真时间戳的对应关系准确。

### 3. 关键文件

*   `iommu/cache_src/common/stats_collector.h/cpp`: 统计数据的定义、收集与文件输出逻辑。
*   `iommu/cache_src/subsystem/cache_subsystem.cpp`: 实现了任务级追踪日志的生成逻辑 (`trace_task_event`) 和调度器间隙分析。
*   `iommu/iommu_perf_model/iommu_perf_collector.cc`: 展示了控制台调试日志的典型用法。
*   `main.cpp`: 仿真入口，负责初始化并触发最终的统计报告生成。