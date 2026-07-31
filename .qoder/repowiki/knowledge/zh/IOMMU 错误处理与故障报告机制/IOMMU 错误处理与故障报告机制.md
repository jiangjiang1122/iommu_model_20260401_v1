---
kind: error_handling
name: IOMMU 错误处理与故障报告机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/cache_src/common/json_config.cpp
---

该仓库采用基于 RISC-V IOMMU 规范（RVI-IOMMU）的硬件级错误处理架构，主要依赖**故障队列（Fault Queue）**和**TLM 响应状态**来传播和处理错误。系统不使用 C++ 异常（`try/catch`）作为核心控制流，仅在配置解析等辅助模块中使用 `std::runtime_error`。

### 1. 核心错误处理系统
*   **故障队列 (Fault Queue, FQ)**：这是 IOMMU 报告错误的核心机制。当发生地址翻译错误、访问违规或数据损坏时，硬件模型会调用 `report_fault` 函数，将错误记录写入内存中的环形缓冲区（Fault Queue）。
*   **故障记录结构 (`fault_rec_t`)**：定义在 `iommu/include/iommu_fault.hh` 中，包含错误原因 (`CAUSE`)、设备 ID (`DID`)、进程 ID (`PID`)、事务类型 (`TTYP`) 以及相关的 IO 虚拟地址 (`iotval`, `iotval2`)。
*   **错误原因编码 (`CAUSE`)**：遵循 RISC-V IOMMU 规范，定义了多种标准错误码，如 `ACCESS_FAULT` (0x01), `DATA_CORRUPTION` (0x02), `GST_PAGE_FAULT` (0x21) 等，以及特定于实现的错误码（如 256-274 用于 DDT/PDT/MSI 相关错误）。

### 2. 关键文件与逻辑
*   **`iommu/include/iommu_fault.hh`**：定义了故障记录的结构体 `fault_rec_t` 和错误原因宏。
*   **`iommu/iommu_fun_model/iommu_faults.cc`**：实现了 `report_fault` 函数。该函数负责检查故障队列状态（是否启用、是否溢出、是否有内存访问错误），并根据 `DTF` (Disable Translation Fault) 位决定是否抑制某些翻译错误的报告。
*   **`iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc`**：在性能模型中，`fault_cq_proc_thread` 线程负责从收集器接收错误任务，调用 `report_fault` 记录到 FQ，并根据错误类型设置 TLM 事务的响应状态（如 `TLM_GENERIC_ERROR_RESPONSE` 或 `TLM_OK_RESPONSE`）。
*   **`iommu/iommu_fun_model/iommu_translate.cc`**：在地址翻译流程的末尾（`stop_and_report_fault` 标签处），根据错误原因和事务类型（ATS vs 非 ATS）决定是生成 Unsupported Request (UR)、Completer Abort (CA) 还是 Success Response (R=W=0)。

### 3. 架构约定与设计决策
*   **错误抑制 (DTF)**：系统支持通过 `DTF` 位禁用部分翻译错误的报告，但严重错误（如数据损坏、内部路径错误）始终会被报告。
*   **队列溢出处理**：如果故障队列满 (`fqof`) 或写入队列时发生内存访问错误 (`fqmf`)，新的故障记录将被丢弃，并设置相应的状态位，同时可能触发中断。
*   **TLM 映射**：在 SystemC/TLM 性能模型中，硬件错误被映射为 TLM 响应状态：
    *   **Completer Abort / Unsupported Request** -> `tlm::TLM_GENERIC_ERROR_RESPONSE`
    *   **ATS Page Fault (Success with R=W=0)** -> `tlm::TLM_OK_RESPONSE` (但数据字段无效)
*   **配置错误处理**：在缓存子系统的配置加载模块 (`iommu/cache_src/common/json_config.cpp`) 中，使用标准的 C++ 异常 (`throw std::runtime_error`) 处理文件打开失败或 JSON 解析错误，这与核心的硬件错误处理路径是分离的。

### 4. 开发者指南
*   **报告新错误**：在翻译逻辑中检测到错误时，应设置 `cause` 变量并跳转到 `stop_and_report_fault` 标签（在功能模型中）或调用 `report_fault` 并设置 TLM 响应状态（在性能模型中）。
*   **错误码选择**：必须严格遵循 `iommu_fault.hh` 中定义的 `CAUSE` 编码，区分访问错误、页面错误和数据损坏。
*   **调试**：可以通过观察 `fqcsr` 寄存器中的 `fqof` (溢出) 和 `fqmf` (内存故障) 位，以及检查内存中的故障队列内容来调试错误处理逻辑。