该 RISC-V IOMMU SystemC 性能仿真模型**未采用专用的日志框架**（如 spdlog、glog 等），而是通过以下三种混合方式实现日志记录与性能数据输出：

### 1. 控制台调试日志 (Console Debug Logs)
*   **实现方式**：直接在代码中使用 `std::cout` 和 `printf` 进行即时输出。
*   **分布**：广泛分布于 `iommu/iommu_perf_model`（如 `iommu_perf_collector.cc`）和 `iommu/cache_src`（如 `pt_cache.cpp`, `walker_cache.cpp`）中。
*   **格式约定**：
    *   使用方括号标签标识模块或事件类型，例如 `[COLLECTOR]`, `[PT_CACHE]`, `[WALKER_CACHE]`, `[DDR_DISPATCH]`。
    *   部分日志包含仿真时间戳，如 `[t=%llu ns][WALKER_CACHE] HIT: ...`。
    *   关键路径日志通常伴随 `fflush(stdout)` 以确保实时性。
*   **特点**：无日志级别管理（Info/Debug/Error），无运行时开关控制，主要用于开发调试和流程追踪。

### 2. 结构化性能统计 (Structured Performance Statistics)
*   **核心组件**：`iommu::StatsCollector` 类 (`iommu/cache_src/common/stats_collector.{h,cpp}`)。
*   **功能**：
    *   **指标收集**：针对 DC/PC/PT/MSIPT 等缓存子系统，自动统计命中率 (Hit/Miss)、驱逐数 (Evictions)、预取准确率、以及各类延迟（执行延迟、排队延迟、请求全生命周期延迟）。
    *   **直方图支持**：内置延迟直方图记录功能 (`record_execution_latency`)。
    *   **阶段细分**：对 PT Cache 等关键模块，区分 REQUEST 和 UPDATE 阶段的性能表现。
*   **输出方式**：通过 `report()` 方法将格式化后的统计摘要和直方图写入指定的输出文件（默认为 `cache_sim.log`）。

### 3. 硬件性能计数器建模 (HPM - Hardware Performance Monitor)
*   **核心组件**：`count_events` 函数 (`iommu/iommu_perf_model/iommu_hpm.cc`) 及 `iommu_hpm.hh`。
*   **功能**：模拟 RISC-V IOMMU 规范定义的硬件性能监控单元 (PMU)。
*   **事件类型**：支持统计未转换请求、TLB Miss、DDT/PDT 页表遍历次数等标准硬件事件。
*   **过滤机制**：支持基于 device_id, process_id, GSCID, PSCID 的硬件事件过滤计数。

### 开发者建议
*   **调试日志**：若需增加调试信息，请沿用 `[MODULE_NAME]` 标签格式的 `printf` 或 `std::cout`。注意在高频路径上避免过多的控制台输出以免影响仿真性能。
*   **性能分析**：应优先使用 `StatsCollector` 记录的 `cache_sim.log` 进行命中率与延迟分析，而非依赖控制台日志。
*   **日志路由**：目前所有日志均指向 stdout 或单一文件，不支持多路路由或动态日志级别切换。