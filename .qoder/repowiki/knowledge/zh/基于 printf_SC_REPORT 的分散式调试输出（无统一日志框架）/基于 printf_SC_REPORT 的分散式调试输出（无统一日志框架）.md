---
kind: logging_system
name: 基于 printf/SC_REPORT 的分散式调试输出（无统一日志框架）
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - Makefile
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/cache_src/cache/pt_cache.cpp
    - iommu/cache_src/cache/walker_cache.cpp
    - ddr/test_ddr.cc
    - rp/test_rp.hh
---

本仓库未实现统一的日志系统。代码中的“日志”全部是分散的、面向调试的原始输出，主要采用以下两种形式：

1. **printf / std::cout 直接输出**
   - 在 `iommu/iommu_fun_model/iommu_msi_trans.cc`、`ddr/test_ddr.cc`、`rp/test_rp.hh`、`rp/test_rp_func.cc`、`pcienoc/test_pcienoc.cc`、`slink/test_slink.cc` 等大量源文件中直接使用 `printf("[MSI_TRANSLATION] ...")`、`printf("[DDR] ...")`、`printf("[RP_RSP] ...")` 等形式打印带前缀标签的行。
   - 在 `iommu/cache_src/cache/pt_cache.cpp`、`walker_cache.cpp` 中大量使用 `std::cout << "[PT_CACHE] ..."` 输出 Cache 命中/缺失、占位符插入与批量更新等细节。
   - 这些输出没有结构化字段定义，也没有统一的格式化器或时间戳格式，仅靠人工约定的 `[模块名]` 前缀区分来源。

2. **SystemC 报告 API（极少使用）**
   - 仅在 `iommu/cache_src/subsystem/cache_subsystem.cpp:189` 处调用了一次 `SC_REPORT_ERROR("CacheSubsystem", "push_fifo must be called from a SystemC process")`，用于在错误路径上报错。
   - 未见 `SC_INFO`、`SC_WARN`、`SC_COUT` 等其他 SystemC 日志宏的使用。

3. **编译期开关控制**
   - `main.cpp` 通过 `#ifdef DEBUG` 集中启用一组 `DEBUG_*` 宏（`DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS`），并在 `Makefile` 中 `-DDEBUG -DDEBUG_TRANSLATION ...` 传入编译器。
   - 例如 `iommu_msi_trans.cc` 中所有 MSI 翻译相关 `printf` 都被 `#ifdef DEBUG_MSITRANS` 包裹；其他模块也遵循类似模式。
   - 这是一种粗粒度的“全开/全关”开关，不存在运行时可切换的 log level 策略。

4. **任务追踪（Task Trace）——唯一接近结构化输出的部分**
   - `cache_subsystem.cpp` 提供 `parse_task_trace_level()` 将字符串参数解析为 `DETAIL/BASIC/OFF` 三级，并通过 `trace_task_event()` 以 `[cache-task] level=... phase=... cache=... op=... task_id=...` 的键值对形式输出。
   - 该机制由全局配置 `cfg.statistics.task_trace_level` 驱动，属于性能/行为追踪而非通用日志框架。

5. **结论**
   - 仓库不存在独立的 logger 库、log level 管理、sink 路由或结构化日志中间件。
   - 所有“日志”均为开发阶段临时添加的 `printf/std::cout` 与少量 `SC_REPORT_*` 调用，配合编译期 `DEBUG_*` 宏进行裁剪。
   - 若需引入统一日志系统，建议在此现有 `DEBUG_*` 宏之上封装一层轻量 logger，并复用 `cache_subsystem.cpp` 中已有的 key=value 风格输出约定作为结构化字段规范。