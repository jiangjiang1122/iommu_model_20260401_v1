---
kind: logging_system
name: 基于 printf + DEBUG 宏的轻量级调试日志系统
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - ddr/test_ddr.cc
    - rp/test_rp.hh
    - iommu/include/iommu_task.hh
---

## 1. 使用的系统/方法

本仓库**没有引入任何第三方日志框架**（如 spdlog、glog、log4cplus 等），也没有统一的日志库或封装。所有输出均通过 C 标准库 `printf` / `fprintf(stdout, ...)` / `fflush(stdout)` 直接打印到标准输出，由运行脚本重定向到 `.log` 文件（例如 `sim_*.log`、`build_*.log`、`reg_*.log`）。

日志开关通过编译期 `#ifdef DEBUG_*` 宏控制：在 `main.cpp` 中，若定义了顶层 `DEBUG`，则自动展开为 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS` 等模块级宏；各源文件中关键路径的 `printf` 调用被包裹在对应的 `#ifdef DEBUG_xxx` 块内，从而可在构建时完全剔除调试输出。

## 2. 关键文件与位置

- **入口与宏定义**：`main.cpp` —— 集中定义 `DEBUG_*` 宏的级联展开，并作为仿真启动/结束的日志点。
- **功能模型中的调试日志**：`iommu/iommu_fun_model/iommu_msi_trans.cc` —— 大量 `printf("[MSI_TRANSLATION] ...")` 按步骤标注 MSI 地址翻译流程，全部受 `DEBUG_MSITRANS` 保护。
- **测试/子系统日志**：
  - `ddr/test_ddr.cc` —— `[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[DDR] ERROR:` 前缀的 DDR 行为日志。
  - `rp/test_rp.hh` —— `[RP_RSP]` 前缀的 AT 响应日志；同时提供 `START_TEST` / `fail_if` / `END_TEST` 测试断言宏，使用 ANSI 颜色码输出 PASS/FAIL。
- **任务结构中的“日志字段”**：`iommu/include/iommu_task.hh` 中 `iommu_task_t` 包含 `ddr_log_type[8]`、`ddr_log_level[8]`、`ddr_log_addr[8]`、`ddr_log_size[8]`、`ddr_log_count` 等字段，用于在内存中记录页表访问的 DDR 日志条目（类型、级别、地址、大小、计数），属于**运行时数据采样**而非文本日志。

## 3. 架构与约定

- **无中心化 logger**：不存在全局 logger 对象、日志级别枚举、sink 注册机制。每个模块自行决定在哪里 `printf`。
- **模块化前缀约定**：日志行以方括号模块名开头，如 `[DDR]`、`[MSI_TRANSLATION]`、`[RP_RSP]`，便于 grep 过滤。
- **调试粒度**：细粒度到算法步骤（如 `Step 1`、`Step 3`、`Step 7`、`Step 13b`），配合 `DEBUG_MSITRANS` 可逐行跟踪 MSI 翻译过程。
- **错误输出**：错误信息同样走 `printf`（如 `"[DDR] ERROR: Address ... exceeds memory size"`），未区分 stderr/stdout。
- **性能统计分离**：真正的“结构化统计”通过 `stats_collector`（`iommu/cache_src/common/stats_collector.{h,cpp}`）和 `iommu_task_t` 中的 `ddr_log_*` 数组收集，最终由外部脚本分析 CSV/文本结果，而不是写入日志流。

## 4. 约定与约束

- **编译期开关**：所有调试日志必须包裹在 `#ifdef DEBUG_xxx` 中，否则会在 release 构建中产生冗余 I/O。该约定由 `main.cpp` 的 `DEBUG` → `DEBUG_*` 宏展开强制统一来源。
- **输出缓冲刷新**：关键路径（如 `main.cpp` 启动阶段、`rp/test_rp.hh` 的 `nb_transport_bw` 回调）显式调用 `fflush(stdout)`，保证 SystemC 事件驱动下日志可见性。
- **无日志级别**：代码中未实现 INFO/WARN/ERROR/FATAL 等多级别分类，仅靠消息内容语义区分。
- **无结构化格式**：日志为人类可读的格式化字符串，非 JSON/CSV；结构化数据通过 `ddr_log_*` 字段数组在内存中累积，供后续分析。
- **测试专用宏**：`rp/test_rp.hh` 中的 `START_TEST` / `fail_if` / `END_TEST` 是测试套件专用的轻量断言+日志宏，不属于通用日志系统。

总结：这是一个面向 SystemC 仿真调试的极简日志方案——用 `printf` + `DEBUG_*` 宏组合实现按需输出的文本日志，辅以任务结构体中的数组字段做运行时采样，依赖外部 shell/python 脚本对输出进行后处理分析。