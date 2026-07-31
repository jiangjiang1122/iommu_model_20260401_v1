---
kind: logging_system
name: 日志系统 — 基于 printf/调试宏的分散式输出
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - ddr/test_ddr.cc
    - rp/test_rp.hh
    - iommu/cache_src/common/stats_collector.cpp
---

该仓库未实现统一的日志框架或结构化日志系统，所有输出均通过 C 标准库 `printf` 与少量 `std::cerr` 直接打印到标准输出/错误流，配合编译期 `DEBUG_*` 宏进行条件开关。具体表现如下：

1. **输出方式**
- 绝大多数模块使用 `printf("[TAG] ...", ...)` 形式，标签如 `[DDR]`、`[MSI_TRANSLATION]`、`[PT_CACHE]`、`[WALKER_CACHE]`、`[DEDUP_CACHE]`、`[RP_RSP]` 等，属于按功能域划分的文本前缀。
- 统计收集器 `stats_collector.cpp` 在文件打开失败时通过 `std::cerr << "Warning: ..."` 输出警告。
- 测试辅助头 `rp/test_rp.hh` 提供 `fail_if` / `END_TEST` 宏，内部同样用 `printf` 输出测试结果。
- `main.cpp` 启动时打印 SystemC 版本、版权信息以及仿真开始/结束提示。

2. **级别控制机制**
- 没有运行时日志级别（INFO/WARN/ERROR 等），仅依赖编译期宏。`main.cpp` 中定义了 `#ifdef DEBUG` 后自动展开 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS` 等宏。
- 被 `#ifdef DEBUG_MSITRANS` 包裹的 `printf` 集中在 `iommu/iommu_fun_model/iommu_msi_trans.cc` 中，用于逐步跟踪 MSI 地址翻译流程；其他模块的 `printf` 大多未被宏保护，始终输出。

3. **结构与路由**
- 无集中式 logger 初始化、无 sink/文件重定向、无异步队列，所有日志直接写入 stdout/stderr。
- 日志内容以人类可读的文本行为主，包含时间戳片段（如 `[t=%llu ns]`）、地址、状态码等字段，但并非 JSON 或其他结构化格式。

4. **约定与约束**
- 代码风格上采用 `[模块名] 描述` 的固定前缀模式，便于 grep 过滤。
- 关键路径（如 MSI 翻译）通过 `DEBUG_*` 宏屏蔽高频调试输出，避免影响性能；非关键路径的 `printf` 则常驻。
- 未见任何第三方日志库（如 spdlog、glog、log4cplus 等）或自定义封装类，日志完全散落在各源文件中。