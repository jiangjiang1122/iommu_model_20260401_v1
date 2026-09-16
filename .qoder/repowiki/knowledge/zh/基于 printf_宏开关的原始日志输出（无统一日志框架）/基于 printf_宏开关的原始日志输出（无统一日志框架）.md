---
kind: logging_system
name: 基于 printf/宏开关的原始日志输出（无统一日志框架）
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - ddr/test_ddr.cc
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - rp/test_rp.hh
---

## 1. 使用的系统/方法

本仓库**没有引入任何第三方日志框架**（如 spdlog、glog、log4cplus、SystemC 自带的 sc_report 等），也没有统一的日志头文件或日志子系统。所有“日志”均通过 C 标准库 `printf` / `fprintf(stdout, ...)` 直接输出到标准输出，配合 `fflush(stdout)` 保证实时刷新。

调试信息通过编译期宏开关控制：在 `main.cpp` 中定义 `DEBUG` 后，会级联展开 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS` 等宏；被 `#ifdef DEBUG_*` 包裹的 `printf` 仅在开启对应宏时编译进二进制。

## 2. 关键文件与位置

- **入口与宏开关**：`main.cpp` —— 集中定义 `DEBUG` → `DEBUG_*` 宏，并打印仿真启动/结束信息。
- **DDR 模型日志**：`ddr/test_ddr.cc` —— 使用 `[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[DDR] ERROR:` 等带前缀的 `printf` 输出内存分配、请求调度、响应时间、地址越界错误等。
- **MSI 翻译调试日志**：`iommu/iommu_fun_model/iommu_msi_trans.cc` —— 大量 `printf("[MSI_TRANSLATION] ...")` 按步骤（Step 1~14f）跟踪 MSI 地址翻译流程，全部受 `#ifdef DEBUG_MSITRANS` 保护。
- **测试断言/结果输出**：`rp/test_rp.hh` —— 定义 `START_TEST`、`fail_if`、`END_TEST` 宏，以及 `[RP_RSP]` 等 `printf`，用于测试用例的 PASS/FAIL 标记和 AT 响应跟踪。
- **顶层模块**：`iommu_top.cc` / `iommu_top.hh`、`pcienoc/test_pcienoc.hh`、`slink/test_slink.cc` 等——未发现额外日志实现，主要依赖上述模块的 `printf`。

## 3. 架构与约定

- **无分层**：不存在 logger 初始化、sink 配置、日志级别枚举、异步写入队列等中间层。每个模块自行决定在哪里调用 `printf`。
- **标签式前缀**：日志行普遍以方括号标签开头，如 `[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[MSI_TRANSLATION]`、`[RP_RSP]`、`Test XX : ...`，便于用 `grep` 过滤特定模块的输出。
- **结构化字段**：日志内容以键值风格嵌入字符串，例如 `addr=0x%lx, arrive=%s, dispatch=%s, queue_wait=%s, rd_out=%d, wr_out=%d`，但这是纯文本格式化而非 JSON/结构化格式。
- **级别策略**：仅存在两种隐式级别——
  - 始终输出的“运行态”日志（如 DDR 分配、AT 响应、测试 PASS/FAIL）；
  - 由 `DEBUG_*` 宏控制的“详细调试”日志（如 MSI 翻译每一步）。未提供运行时切换级别的机制。
- **同步输出**：每次 `printf` 后紧跟 `fflush(stdout)`，避免缓冲导致时序失真，适合 SystemC 事件驱动仿真中对精确时间戳的需求。

## 4. 约定与约束

- **约束来源**：`main.cpp` 中的 `#ifdef DEBUG` 块强制要求所有细粒度调试输出必须通过 `DEBUG_*` 宏包裹，否则无法编译启用；这构成唯一的“可编译期开关”约束。
- **约定**：
  - 模块日志统一使用 `[MODULE_NAME]` 或 `[SUBSYSTEM_STEP]` 形式的方括号前缀，方便 post-processing（仓库根目录有大量 `*.sh`、`*.py` 分析脚本正是基于这种格式解析日志）。
  - 错误路径使用 `[MODULE] ERROR:` 前缀（见 `ddr/test_ddr.cc` 的地址越界输出）。
  - 测试代码使用 `START_TEST` / `fail_if` / `END_TEST` 三件套作为统一测试输出约定。
- **缺失能力**：无日志级别枚举、无 sink 路由（无法将不同模块日志分流到不同文件或 fd）、无结构化序列化、无性能开销统计、无线程安全封装。所有输出均为阻塞式 stdout。

## 5. 结论

该仓库的“日志系统”本质上是**散落的 `printf` + 编译期 `DEBUG_*` 宏开关**，没有任何抽象层或框架。它满足功能仿真调试的基本需求（可读的前缀标签、步骤化输出、错误标记），但不具备生产级日志系统的特性（级别管理、结构化、多 sink、异步）。如果需要扩展，应首先考虑引入统一日志头文件以集中管理级别与输出目标。