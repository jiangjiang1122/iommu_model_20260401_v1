---
kind: error_handling
name: IOMMU 性能模型错误处理与故障报告机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/iommu_perf_model/iommu_perf_ptw.cc
    - iommu/iommu_reg.cc
    - iommu/iommu_top.cc
    - iommu/include/iommu_registers.hh
    - iommu/cache_src/cache/cache_base.h
---

该 IOMMU 性能模型（基于 SystemC/TLM）采用**状态驱动的错误传播**和**硬件寄存器级故障报告**机制，而非传统的软件异常（Exception）或断言（Assert）机制。其核心设计理念是模拟硬件在地址转换失败时的行为：将错误封装为任务状态，并通过 Fault Queue (FQ) 或调试接口向软件报告。

### 1. 核心错误处理架构

*   **状态机驱动 (State-Driven)**：
    *   所有翻译任务 (`iommu_task_t`) 携带一个 `state` 字段。正常路径为 `TASK_INIT` -> `TASK_PTW_REQ` -> `TASK_PTW_DONE` -> `TASK_DONE`。
    *   错误路径则跳转到 `TASK_FAULT`。一旦进入 `TASK_FAULT`，任务不再进行后续的物理地址转换，而是直接进入故障收集阶段。
*   **故障原因编码 (Cause Codes)**：
    *   使用 `task->cause` 字段存储 RISC-V IOMMU 规范定义的故障代码（如 12: Instruction access fault, 13: Load access fault, 15: Store/AMO access fault, 21: Guest page fault 等）。
    *   在页表遍历 (PTW) 过程中，任何 PTE 合法性检查失败（如 V=0, R=0&W=1, 权限不足, 地址非规范等）都会立即设置 `cause` 并标记 `walk_fault = true`。

### 2. 关键错误检测点

*   **页表遍历 (PTW) 错误** (`iommu_perf_ptw.cc`)：
    *   **Canonical Check**: 检查 IOVA 是否符合当前模式（Sv39/48/57）的规范地址要求。
    *   **PTE 合法性**: 检查 PTE 的 V (Valid), R/W/X (权限), PBMT, Reserved 位。
    *   **超级页对齐**: 检查非叶子节点或超级页的 PPN 低位是否为零。
    *   **A/D 位更新**: 如果硬件不支持原子更新 (SADE/GADE=0) 且需要设置 A/D 位，则触发故障。
*   **寄存器访问错误** (`iommu_reg.cc`)：
    *   **非法访问**: `is_access_valid` 函数检查访问对齐、大小和边界。非法访问通常被静默丢弃（读返回 0，写忽略），模拟硬件的 UNSPECIFIED 行为。
    *   **忙状态保护**: 对 `ddtp`, `cqcsr`, `fqcsr` 等寄存器的写入会检查 `busy` 位。如果硬件忙，写入被丢弃，防止状态竞争。
    *   **状态依赖**: 例如，在队列启用时修改基地址寄存器 (`cqb`, `fqb`) 会被拒绝。

### 3. 故障报告与传播

*   **Fault Queue (FQ)**：
    *   当任务处于 `TASK_FAULT` 状态时，会被写入 `collector_to_fault_fifo`。
    *   故障收集器线程负责将故障记录写入内存中的 Fault Queue，并设置 `fqcsr.fqof` (Overflow) 或 `fqmf` (Memory Fault) 状态位。
    *   如果 Fault Queue 禁用或溢出，故障可能被丢弃或触发中断。
*   **调试接口 (Translation Request)**：
    *   通过 `tr_req_ctrl` 和 `tr_req_iova` 寄存器发起的调试翻译，其结果直接写入 `tr_response` 寄存器。
    *   `tr_response.fault` 位指示是否发生故障，供软件轮询检查。
*   **TLM 响应状态**：
    *   在 TLM 接口层 (`iommu_top.cc`)，如果发生严重内部错误（如扩展信息缺失），会设置 `trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE)`。
    *   DDR 访问错误通过 `rsp.error` 标志传递，并在 PTW 响应线程中检查。

### 4. 开发者约定与规则

*   **禁止使用 C++ 异常**：代码库中极少使用 `try-catch`（仅在 Cache 底层 RAM 访问包装器中用于资源清理）。业务逻辑严禁抛出异常，必须通过返回状态码或设置 `task->state` 来处理错误。
*   **静默失败原则**：对于寄存器非法访问，遵循 RISC-V IOMMU 规范，通常采取“丢弃写入/读零”策略，而不是崩溃或报错，以模拟真实硬件行为。
*   **错误日志**：使用 `printf` 配合 `[PTW_RSP] ... FAULT` 或 `[DDR] ERROR` 前缀进行调试日志记录。生产环境中应通过 Fault Queue 获取错误信息。
*   **资源泄漏防护**：在 PEQ (Primitive Event Queue) 和 FIFO 通信中，动态分配的对象（如 `ddr_rsp_entry_t*`）必须在消费后显式 `delete`，或在栈上分配以避免泄漏。