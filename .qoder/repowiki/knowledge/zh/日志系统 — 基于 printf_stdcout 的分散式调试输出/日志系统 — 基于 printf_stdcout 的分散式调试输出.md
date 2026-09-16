---
kind: logging_system
name: 日志系统 — 基于 printf/std::cout 的分散式调试输出
category: logging_system
scope:
    - '**'
source_files:
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/cache_src/cache/pt_cache.cpp
    - iommu/cache_src/cache/walker_cache.cpp
    - iommu/cache_src/cache/dedup_cache.cpp
    - iommu/cache_src/common/stats_collector.cpp
    - ddr/test_ddr.cc
    - rp/test_rp.hh
---

本仓库未实现统一的日志框架或结构化日志系统。所有调试与诊断输出均通过 C 标准库 `printf`/`fprintf` 和 C++ 标准流 `std::cout`/`std::cerr` 直接打印到标准输出/错误，属于典型的“散点式”调试输出模式。

**使用方式与分布**
- 核心 IOMMU 功能模块（如 `iommu/iommu_fun_model/iommu_msi_trans.cc`）使用 `printf("[MSI_TRANSLATION] ...", ...)` 形式，以方括号前缀标识子系统。
- 缓存子系统（`iommu/cache_src/cache/*.cpp`，如 `pt_cache.cpp`、`walker_cache.cpp`、`dedup_cache.cpp`）大量使用带时间戳 `[t=%llu ns][MODULE]` 格式的 `printf` 输出，包含任务 ID、地址、状态等字段。
- 统计收集器 `iommu/cache_src/common/stats_collector.cpp` 通过 `std::cerr` 输出文件打开失败的警告信息。
- 测试与脚本目录（`ddr/`、`rp/`、`tmp/` 下的 `.md` 文档中的示例代码）也散布着 `printf`/`std::cout` 调用。

**结构与约定**
- 无统一 logger 头文件或初始化入口；每个模块自行决定输出格式。
- 输出内容多为面向人类阅读的调试信息，而非结构化机器可读格式（无 JSON、无固定 schema）。
- 没有日志级别管理（INFO/WARN/ERROR/FATAL），仅靠消息文本中的关键词（如 `ERROR`、`Warning`、`FAIL`）区分严重性。
- 没有日志路由、去重、缓冲或异步写入机制，所有输出同步阻塞。

**约束与限制**
- 由于是纯 SystemC/TLM 性能模型，未见使用 SystemC 自带的 `sc_info`/`sc_warning`/`sc_error` 等宏。
- 输出格式不统一：部分含纳秒时间戳，部分不含；部分带彩色 ANSI 转义码（如 `rp/test_rp.hh` 中的 `\x1B[31m`/`\x1B[32m`），不利于自动化解析。
- 无日志开关或动态级别控制，无法在运行时关闭或调整输出粒度。

综上，该仓库的“日志系统”实质上是零散的 `printf`/`std::cout` 调试语句集合，不具备可配置、可路由、结构化的日志能力。