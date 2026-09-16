---
kind: logging_system
name: 日志系统 — 基于 printf/SC_REPORT 的调试输出与统计报告
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - ddr/test_ddr.cc
    - rp/test_rp.hh
---

本仓库未引入专用日志框架（如 spdlog、glog、nlohmann::json 等），而是采用 C/C++ 标准 I/O 与 SystemC 内置报告机制的组合方式实现调试与统计输出。

1. 使用的系统与工具
- 标准输出：大量使用 `printf` / `std::cout` / `std::cerr` 直接打印调试信息，格式为 `[模块名] ...` 的前缀风格，例如 `[PT_CACHE]`、`[WALKER_CACHE]`、`[DDR_DISPATCH]`、`[MSI_TRANSLATION]` 等。
- SystemC 报告：在关键路径中使用 `SC_REPORT_ERROR` 进行错误上报（如 cache_subsystem.cpp 中对 push_fifo 调用上下文的检查）。
- 文件输出：通过 `StatsCollector` 将统计结果写入指定文件，并支持直写 flush。

2. 关键文件与位置
- main.cpp：仿真入口，仅用 `printf` 打印启动/结束信息。
- iommu/cache_src/common/stats_collector.cpp：统一的统计收集与报告输出，提供 `set_output_file()`、`write_log()`、`report()` 接口，内部以 `std::ofstream` 写入文件，失败时通过 `std::cerr` 警告。
- iommu/cache_src/subsystem/cache_subsystem.cpp：使用 `SC_REPORT_ERROR` 进行运行时错误上报。
- iommu/iommu_fun_model/iommu_msi_trans.cc：大量 `#ifdef DEBUG_MSITRANS` 包裹的 `printf` 调试输出。
- ddr/test_ddr.cc、rp/test_rp.hh 等测试模块：同样使用 `printf` 作为主要输出手段。

3. 架构与约定
- 无全局 logger 单例或集中式初始化；每个模块自行决定输出位置与格式。
- 调试开关通过编译期宏控制：Makefile 中默认开启 `-DDEBUG -DDEBUG_TRANSLATION -DDEBUG_TWOSTAGE -DDEBUG_MSITRANS -DDEBUG_COMMANDS -DDEBUG_SECONDSTAGE -DDEBUG_ATC -DDEBUG_FAULTS -DDEBUG_INTERRUPT -DDEBUG_HPM -DDEBUG_UTILS`，各模块内用 `#ifdef DEBUG_*` 包裹对应 `printf` 段。
- 输出前缀约定：`[模块名]` 形式（如 `[PT_CACHE_LAZY_INVAL]`、`[WALKER_S2_CACHE]`），便于快速定位来源。
- 时间戳：部分缓存模块在输出中嵌入 `[t=%llu ns]` 形式的仿真时间戳。
- 结构化字段：通过格式化字符串拼接关键字段（task_id、iova、gscid、pscid、level、next_ppn 等），但未采用 JSON 或其他结构化格式。

4. 约定与约束
- 所有调试输出均为文本行，无统一日志级别（INFO/WARN/ERROR 等）抽象，依赖前缀和上下文区分语义。
- 性能相关统计由 `StatsCollector` 统一管理，通过 `set_output_file(path)` 指定输出文件，`report()` 生成汇总与直方图，失败时回退到 `std::cerr`。
- 错误处理优先使用 `SC_REPORT_ERROR`（SystemC 标准），其他异常场景使用 `std::cerr << "Warning: ..."`。
- 无异步日志、无多 sink 路由、无日志轮转；输出均为同步阻塞 I/O。
- 测试代码中的断言宏（如 `fail_if`、`END_TEST`）也通过 `printf` 输出 PASS/FAIL 标记。