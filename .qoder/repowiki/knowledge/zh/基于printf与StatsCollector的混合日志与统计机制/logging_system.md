该仓库采用了一种**非结构化、基于标准输出流（stdout）的调试日志**与**内存内结构化性能统计（StatsCollector）**相结合的混合观测机制。系统中不存在独立的日志框架（如spdlog、glog等），也未实现日志级别管理或文件轮转策略。

### 1. 核心日志方式：C风格 printf
- **实现方式**：在 `main.cpp`、`iommu_top.cc` 以及各个功能模块（如 `ddr/test_ddr.cc`）中，广泛使用 C 语言标准的 `printf` 函数进行运行时状态输出。
- **内容特征**：日志内容高度定制化，通常包含仿真时间戳、任务ID（task_id）、物理/虚拟地址（iova/pa）以及模块内部状态机跳转信息。例如：`[IOMMU_TOP] axi_slave_nb_transport_fw: task_id=%u...`。
- **同步控制**：为了确保 SystemC 仿真事件与日志输出的时序一致性，关键路径上的 `printf` 后通常会紧跟 `fflush(stdout)`，防止缓冲区延迟导致日志乱序。
- **条件编译**：部分调试日志通过 `#ifdef DEBUG` 宏进行控制，但大部分性能追踪日志是默认开启的。

### 2. 性能统计系统：StatsCollector
- **架构位置**：位于 `iommu/cache_src/common/stats_collector.{h,cpp}`，是缓存子系统（CacheSubsystem）的核心观测组件。
- **数据结构**：通过 `CacheStats` 结构体维护多维度的计数器，包括命中/缺失次数、驱逐数、预取准确率、以及分阶段的延迟直方图（Execution Latency, Queue Latency）。
- **输出机制**：
  - **控制台汇总**：在仿真结束前（`print_cache_statistics`），通过 `std::cout` 打印格式化的统计表格。
  - **文件持久化**：支持通过 `set_output_file("cache_sim.log")` 将详细的统计报告和直方图数据写入指定的文本文件。
- **细粒度追踪**：针对 PT Cache 等复杂模块，实现了按阶段（REQUEST/UPDATE/MONITOR）拆分的时间戳记录，用于计算“任务放大系数”和端口利用率。

### 3. 错误报告机制
- **SystemC 原生报告**：仅在极少数严重违反 SystemC 调度规则的场景下（如在非进程上下文中调用 FIFO 操作）使用 `SC_REPORT_ERROR`。
- **手动错误标识**：大多数逻辑错误或故障注入（Fault Injection）通过修改任务状态字段（如 `task->cause`）并配合 `printf` 输出来实现，而非抛出异常或调用日志框架的错误接口。

### 4. 开发者规范与建议
- **日志添加**：新增调试信息时，应遵循 `[MODULE_NAME] description: key=value` 的格式，并在涉及跨线程/FIFO 交互时务必调用 `fflush(stdout)`。
- **性能观测**：若需观测缓存行为，应优先调用 `StatsCollector` 提供的 `record_hit/miss/latency` 接口，避免在高频执行路径（如每周期执行的逻辑）中添加繁重的 `printf`，以免显著拖慢 SystemC 仿真速度。
- **清理策略**：由于缺乏自动化的日志清理工具，产生的 `.log` 文件需通过 `.gitignore` 忽略，并由开发者手动管理磁盘空间。