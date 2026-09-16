---
kind: logging_system
name: 基于 printf/标准输出的调试日志（无统一日志框架）
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - rp/test_rp.hh
    - ddr/test_ddr.cc
    - iommu/iommu_fun_model/iommu_msi_trans.cc
---

## 1. 使用的系统/方法

本仓库**没有引入任何第三方日志框架或统一的日志子系统**。所有运行时输出均通过 C 标准库的 `printf` / `fflush(stdout)` 直接写入标准输出，配合 SystemC 仿真时间推进。代码中未发现 `std::cout`、`spdlog`、`glog`、`LOG_*` 宏等结构化日志设施。

## 2. 关键文件与位置

- `main.cpp`：仿真入口，使用 `printf` 打印 SystemC 版本、启动/结束信息，并在每次输出后调用 `fflush(stdout)` 强制刷新缓冲。
- `rp/test_rp.hh`：测试驱动模块，定义了 `START_TEST` / `fail_if` / `END_TEST` 三个宏，内部通过 `printf` 输出带 ANSI 颜色码（\x1B[31m FAIL、\x1B[32m PASS）的测试结果；同时在 `nb_transport_bw` 回调中用 `printf("[RP_RSP] ...")` 打印 AT 响应。
- `ddr/test_ddr.cc`：DDR 模型模块，使用 `printf("[DDR] ...")`、`printf("[DDR_DISPATCH] ...")`、`printf("[DDR_RESP] ...")` 等带前缀的调试行，并包含错误路径 `printf("[DDR] ERROR: ...")`。
- `iommu/iommu_fun_model/iommu_msi_trans.cc`：MSI 地址翻译函数，全篇以 `printf("[MSI_TRANSLATION] Step N: ...")` 形式逐步骤输出中间状态，用于定位 MSI PTE 解析流程。
- 其他 `.cc` 源文件（如 `pcienoc/test_pcienoc.cc`、`slink/test_slink.cc` 以及 `rp/*.cc`）同样散落着 `printf` 调用。

## 3. 架构与约定

- **无集中式 logger**：不存在独立的 `logger.h`、`logging/` 目录或全局日志对象。每个模块自行决定在何处 `printf`。
- **前缀标签约定**：日志行普遍采用 `[MODULE]` 形式的方括号前缀来标识来源，例如 `[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[MSI_TRANSLATION]`、`[RP_RSP]`，便于从混合输出中快速区分来源模块。
- **调试开关**：`main.cpp` 顶部通过 `#ifdef DEBUG` 批量定义 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS` 等宏，但当前源码中并未出现对这些宏的条件编译包裹 `printf` 的逻辑——这些宏目前仅作为“已启用”的标志存在，未实际被消费。
- **输出刷新策略**：在 `main.cpp` 和 `rp/test_rp.hh` 的关键路径上显式调用 `fflush(stdout)`，确保 SystemC 仿真过程中输出及时可见，避免缓冲延迟导致时序分析困难。
- **错误输出**：错误类日志与普通调试日志混用同一 `stdout`，未见重定向到 `stderr` 的约定。

## 4. 约定与约束

- **描述性约定**：
  - 日志行以 `[模块名]` 方括号前缀开头，便于 grep 过滤特定模块的输出。
  - 关键路径（仿真开始/结束、AT 响应、DDR 分发/响应、MSI 翻译各 step）均有 `printf` 覆盖，形成“按步骤”的追踪风格。
  - 测试用例统一通过 `START_TEST` / `fail_if` / `END_TEST` 宏输出，失败时附带 ANSI 红色高亮和行号。
- **可观察到的约束**：
  - 所有日志均为**文本格式**，无 JSON/XML 等结构化字段，也无统一的 log level（INFO/WARN/ERROR）枚举。
  - 日志级别控制依赖外部手段（如 shell 重定向、grep），而非代码内运行时开关。
  - 由于缺少条件编译包裹，`DEBUG_*` 宏目前未起到关闭日志的作用；若需禁用某类日志，需在源码中手动添加 `#ifdef` 包裹。
- **缺失项**：
  - 无异步日志、无日志轮转、无 sink 抽象、无性能计数器集成（统计指标通过独立 CSV/脚本产出，见 `tmp/` 下的分析脚本）。
  - 无统一的日志初始化入口，`main.cpp` 不创建任何 logger 实例。

综上，该仓库的“日志系统”实质上是**散落在各模块中的 `printf` + 固定前缀约定的调试输出**，适合开发期交互式排查，但不具备生产级日志系统的特性（分级、结构化、可路由）。