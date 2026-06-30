该 RISC-V IOMMU 仿真平台采用基于 **RISC-V IOMMU 规范** 的硬件级错误处理机制，主要通过 **Fault Queue（故障队列）**、**寄存器状态位** 和 **TLM 响应状态** 来定义、传播和呈现错误。

### 1. 核心系统与模式
*   **规范驱动的错误模型**：严格遵循 RISC-V IOMMU 规范，定义了详细的故障原因代码（Cause Codes），如 `ACCESS_FAULT` (0x01), `DATA_CORRUPTION` (0x02), `GST_PAGE_FAULT` (0x21) 等。
*   **Fault Queue (FQ)**：作为核心的错误报告通道。当地址翻译或设备上下文查找失败时，系统调用 `report_fault` 将故障记录（`fault_rec_t`）写入内存中的故障队列，并触发中断通知软件。
*   **TLM 错误映射**：在 SystemC/TLM 性能模型中，硬件故障被映射为 TLM 通用负载的响应状态，如 `TLM_GENERIC_ERROR_RESPONSE`（对应 Completer Abort 或 Unsupported Request）。

### 2. 关键文件与逻辑
*   **`iommu/include/iommu_fault.hh`**：定义了故障记录结构 `fault_rec_t` 和标准 Cause 代码宏。
*   **`iommu/iommu_fun_model/iommu_faults.cc`**：实现了 `report_fault` 函数。该函数负责检查 Fault Queue 状态（是否满、是否禁用），构造故障记录，并通过 `write_memory` 将其存入 DDR。如果写入 FQ 本身失败，会设置 `fqmf` (Fault Queue Memory Fault) 位。
*   **`iommu/iommu_fun_model/iommu_translate.cc`**：地址翻译的主逻辑。使用 `goto stop_and_report_fault` 模式集中处理错误。根据故障类型和请求类型（ATS vs Non-ATS），决定是上报 Fault Queue 还是直接返回错误响应。
*   **`iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc`**：性能模型中的故障处理线程 `fault_cq_proc_thread`。它从 FIFO 接收故障任务，调用 `report_fault`，并根据 Cause 代码设置 TLM 响应状态（如 `TLM_OK_RESPONSE` 用于某些 ATS 页面故障，`TLM_GENERIC_ERROR_RESPONSE` 用于访问故障）。
*   **`iommu/include/iommu_registers.hh`**：定义了 `fqcsr_t` (Fault Queue Control/Status Register)，包含 `fqmf` (Memory Fault), `fqof` (Overflow), `fqen` (Enable) 等关键控制位。

### 3. 架构约定与设计决策
*   **早期退出与集中报告**：在翻译流水线中，一旦检测到错误（如 DDT 查找失败、权限检查不通过），立即跳转到 `stop_and_report_fault` 标签，避免无效计算。
*   **ATS 与非 ATS 的区别处理**：
    *   **Non-ATS**：大多数错误会导致 `UNSUPPORTED_REQUEST` 响应，并向 Fault Queue 写入记录。
    *   **ATS**：某些配置错误（如 Access Fault）会导致 `COMPLETER_ABORT`；而页面缺失（Page Fault）则可能返回 `SUCCESS` 但权限位为 0，且不写入 Fault Queue（由设备驱动处理）。
*   **静默丢弃与溢出保护**：如果 Fault Queue 已满 (`fqof=1`) 或发生内存访问错误 (`fqmf=1`)，新的故障记录会被静默丢弃，防止系统死锁。
*   **无异常抛出**：代码库中未使用 C++ 异常（`throw/catch`）。错误通过返回值（`uint8_t status`）、全局状态寄存器或 TLM 响应状态进行传播。

### 4. 开发者规则
*   **故障上报**：在新增翻译逻辑时，若检测到非法状态，应设置 `cause` 变量并跳转至统一的错误处理块，严禁忽略错误继续执行。
*   **TLM 状态映射**：在性能模型中，必须根据规范正确映射 Cause 代码到 TLM 响应状态。例如，`cause=260` (Transaction type disallowed) 应映射为 `TLM_GENERIC_ERROR_RESPONSE`。
*   **队列状态检查**：在模拟 `write_memory` 写入 Fault Queue 时，必须检查 `fqcsr` 寄存器状态，模拟硬件的溢出和访问错误行为。