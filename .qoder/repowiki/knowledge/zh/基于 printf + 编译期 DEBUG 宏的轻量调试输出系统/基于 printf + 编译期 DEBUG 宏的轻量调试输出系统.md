---
kind: logging_system
name: 基于 printf + 编译期 DEBUG 宏的轻量调试输出系统
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - ddr/test_ddr.cc
    - rp/test_rp.hh
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/iommu_perf_model/iommu_perf_collector.cc
    - iommu/iommu_top.cc
---

## 1. 使用的系统/方法

仓库没有引入任何第三方日志框架（如 spdlog、glog、Boost.Log），也没有统一的 logger 抽象层。整个项目的“日志系统”由以下两部分组成：
- **运行时标准输出**：直接使用 `printf`（C 风格）和 `std::cout` / `std::cerr`（C++ 流）向 stdout/stderr 打印调试信息。
- **编译期开关**：通过 `DEBUG` 宏在 `main.cpp` 中展开为多个子模块级调试宏（`DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS`），所有关键路径的 `printf` 调用都被 `#ifdef DEBUG_*` 包裹，从而可在 release 构建中完全消除日志开销。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `main.cpp` | 定义全局 `DEBUG` 宏并展开为各子系统 DEBUG_* 宏；启动时打印 SystemC 版本与仿真起止信息 |
| `iommu/iommu_fun_model/iommu_msi_trans.cc` | 最典型的示例：MSI 地址翻译每一步都用 `#ifdef DEBUG_MSITRANS` + `[MSI_TRANSLATION] ...` 前缀的 `printf` 记录详细中间状态 |
| `ddr/test_ddr.cc` | DDR 模型使用不带编译开关的 `printf`，以 `[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[DDR] ERROR:` 等固定前缀输出 |
| `rp/test_rp.hh` | 测试驱动中的断言辅助宏：`printf("Test %02d : %-40s : ", ...)`、`fail_if`、`END_TEST()` |
| `iommu/cache_src/common/stats_collector.cpp` | 唯一使用 `std::cerr` 的地方：统计报告写入失败时输出 Warning |
| `iommu/iommu_perf_model/iommu_perf_collector.cc` | 性能采集器使用 `std::cout << "[DC Lookup] task_id=..."` 等结构化字段输出 |
| `iommu/iommu_top.cc` | 汇总缓存统计：`cache_sub.stats().print_summary(std::cout)` |

## 3. 架构与约定

### 3.1 无中心化日志库
项目不存在 `log/`、`logging/` 目录，也没有 `Logger`、`LogManager`、`LOG_INFO` 之类的统一入口。每个模块自行决定如何输出。

### 3.2 编译期分级开关
`main.cpp` 中的逻辑是唯一的集中点：
```cpp
#ifdef DEBUG
#ifndef DEBUG_MSITRANS
#define DEBUG_MSITRANS
#endif
#ifndef DEBUG_TRANSLATION
#define DEBUG_TRANSLATION
#endif
// ...
#endif
```
这意味着：
- Release 构建（未定义 `DEBUG`）下，所有 `#ifdef DEBUG_MSITRANS` 块被编译器直接剔除，零开销。
- Debug 构建需通过编译选项（Makefile / CMake）定义 `DEBUG`，再按需开启各子模块。
- 新增子系统应仿照现有模式添加对应的 `DEBUG_XXX` 宏。

### 3.3 结构化字段约定（非正式）
虽然并非通过 JSON 或键值对实现，但代码遵循了人类可读的结构化约定：
- **模块前缀**：用方括号标注来源，如 `[MSI_TRANSLATION]`、`[DDR]`、`[DDR_DISPATCH]`、`[DDR_RESP]`、`[DC Lookup]`、`[PC Lookup]`、`[SEND_REQ_COUT]`。
- **关键字段内联**：在 `printf` 格式串中直接嵌入 `addr=0x%lx`、`task_id=`、`status=`、`cause=` 等键值形式，便于后续用 grep/awk 解析。
- **错误路径显式标记**：错误输出使用 `[DDR] ERROR:` 等前缀，与正常流程区分。

### 3.4 统计输出与日志分离
`StatsCollector` 负责将缓存命中率、延迟直方图等指标写入指定文件（通过 `set_output_file` + `write_log` + `report`），错误时回退到 `std::cerr`。这与调试日志（stdout）是两条独立通道。

## 4. 约定与约束

| 规则 | 说明 | 依据 |
|---|---|---|
| 核心算法路径必须用 `#ifdef DEBUG_XXX` 包裹 | 避免 release 构建产生 I/O 开销 | `iommu_msi_trans.cc` 中每步翻译都如此 |
| 调试输出使用固定方括号前缀标识来源 | 便于脚本过滤 | 全仓 `printf` 均带 `[MODULE]` 风格前缀 |
| 错误信息走 `std::cerr` | 仅统计模块用于报告文件打开失败 | `stats_collector.cpp:547,560` |
| 性能/统计结果单独写文件 | 不混入 stdout 调试日志 | `StatsCollector::set_output_file/write/report` |
| 测试用例使用 ANSI 颜色宏 | `\x1B[31m` FAIL、`\x1B[32m` PASS | `rp/test_rp.hh` 中 `fail_if` / `END_TEST` |

## 5. 结论

该仓库的“日志系统”本质上是**散落的 printf + 编译期 DEBUG 宏**，没有统一的框架、级别体系或 sink 路由。它满足功能验证阶段的调试需求，但不具备生产级日志系统的特性（结构化格式、可配置级别、异步写入、多目标输出）。若未来需要扩展，建议参考现有 `DEBUG_XXX` 模式新增模块级开关，并保持 `[MODULE] key=value` 的前缀约定以便脚本分析。