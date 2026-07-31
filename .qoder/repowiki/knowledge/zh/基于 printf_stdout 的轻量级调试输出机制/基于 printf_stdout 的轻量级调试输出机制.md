---
kind: logging_system
name: 基于 printf/stdout 的轻量级调试输出机制
category: logging_system
scope:
    - '**'
source_files:
    - main.cpp
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/cache_src/cache/walker_cache.cpp
    - iommu/cache_src/cache/dedup_cache.cpp
    - iommu/cache_src/cache/pt_cache.cpp
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - ddr/test_ddr.cc
    - rp/test_rp.hh
---

该仓库未实现专门的日志框架或结构化日志系统，而是采用最基础的 C/C++ 标准输出作为调试与诊断手段。具体表现为：

1. **输出方式**：所有模块（IOMMU 功能模型、缓存子系统、DDR/PCIe/NOC 测试模块）均直接使用 `printf` 和 `std::cout` 向标准输出打印信息，未引入任何第三方日志库（如 spdlog、glog、Boost.Log 等），也未定义统一的 LOG_* 宏。

2. **消息格式约定**：各模块通过方括号前缀标识来源，例如 `[DDR]`、`[MSI_TRANSLATION]`、`[WALKER_CACHE]`、`[PT_CACHE]`、`[DEDUP_CACHE]`、`[CACHE_SUBSYSTEM]`、`[PT_SCHED]`、`[DEDUP_HASH_BP]`、`[RP_RSP]` 等，便于在混合输出中快速定位消息来源。Walker Cache 相关日志还统一包含仿真时间戳 `[t=%llu ns]`，用于时序分析。

3. **级别控制**：没有全局的日志级别开关，但 `main.cpp` 通过 `#ifdef DEBUG` 预编译宏批量启用若干 `DEBUG_*` 宏（如 `DEBUG_MSITRANS`、`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_SECONDSTAGE`、`DEBUG_COMMANDS`），这些宏通常配合条件编译来裁剪调试输出，属于构建期而非运行期的级别管理。

4. **结构化字段**：日志内容以人类可读的文本形式呈现，包含关键上下文字段（如地址、时间、任务 ID、命中/缺失状态、错误原因码等），但未采用 JSON 或其他机器可解析的结构化格式；统计类输出由独立的 `stats_collector` 组件生成 CSV/文本文件，与调试日志分离。

5. **刷新策略**：`main.cpp` 在关键启动点显式调用 `fflush(stdout)` 确保输出及时可见，其余位置依赖 `printf` 默认行为。

6. **约束与限制**：由于缺乏集中式日志基础设施，无法实现运行时动态调整输出级别、多目标路由（文件/网络/控制台）、异步写入或性能开销隔离；大量 `printf` 直接嵌入核心路径（如 walker_cache.cpp 的 HIT/MISS/UPDATE 分支）会在高负载仿真中产生显著 I/O 开销，这也是当前架构的一个已知瓶颈。