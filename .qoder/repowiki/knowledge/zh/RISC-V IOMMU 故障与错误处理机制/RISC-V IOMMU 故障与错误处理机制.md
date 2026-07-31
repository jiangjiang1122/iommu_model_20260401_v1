---
kind: error_handling
name: RISC-V IOMMU 故障与错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_fault.hh
    - iommu/include/iommu_dedup_params.hh
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/iommu_fun_model/iommu_second_stage_trans.cc
    - iommu/include/iommu_perf_model.hh
---

该仓库是一个基于 SystemC/TLM 的 RISC-V IOMMU 系统级仿真模型，其错误处理围绕 **RISC-V IOMMU 规范定义的 Fault Queue（故障队列）机制**展开，而非采用 C++ 异常或统一错误码体系。核心设计如下：

## 1. 故障记录与上报路径
- 所有翻译/访问异常通过统一的 `report_fault()` 函数集中上报（`iommu/iommu_fun_model/iommu_faults.cc`），由调用方传入 CAUSE、iotval、TTYP、DTF 等字段。
- 故障记录结构 `fault_rec_t`（`iommu/include/iommu_fault.hh`）严格遵循规范位域布局（CAUSE/PID/PV/PRIV/TTYP/DID/custom/reserved/iotval/iotval2），并以 32 字节写入内存中的 Fault Queue。
- 各功能模块（ATS、MSI 翻译、两阶段翻译、单阶段翻译等）在检测到异常时直接调用 `report_fault()`，不返回错误码，也不抛出异常。

## 2. DTF（Disable Translation Fault）过滤策略
- `report_fault()` 内部根据 `dtf` 标志和 CAUSE 值决定是否丢弃故障记录：当 DTF=1 时，仅保留“非翻译相关”的 CAUSE（如 256~273），其余页表/地址翻译类故障被静默丢弃，符合规范 Table 8 行为。

## 3. Fault Queue 状态机与反压
- 入队前检查 FQCSR 的 `fqon/fqen`（是否启用）、`fqmf`（上次写失败）、`fqof`（溢出）三位；任一条件为真则直接丢弃并返回。
- 若队列满（`fqt == (fqh - 1)`），置 `fqof=1` 并通过 `generate_interrupt(FAULT_QUEUE)` 触发中断，后续记录全部丢弃直到软件清零 `fqof`。
- 写入队列内存时若返回 ACCESS_FAULT 或 DATA_CORRUPTION，置 `fqmf=1` 并同样触发中断，此后不再产生新记录。

## 4. 故障原因编码（CAUSE）
- 使用一组宏常量定义标准 CAUSE 值：`RVI_IOMMU_ACCESS_FAULT(0x01)`、`RVI_IOMMU_DATA_CORRUPTION(0x02)`、`RVI_IOMMU_GST_PAGE_FAULT(0x21)`、`RVI_IOMMU_GST_ACCESS_FAULT(0x22)`、`RVI_IOMMU_GST_DATA_CORRUPTION(0x23)` 等，覆盖 S/VS/G-stage 各类页表/访问异常。
- 性能模型侧通过 `set_guest_fault_cause()` 辅助函数将基础 fault 映射到 guest-page-fault 系列（20/21/23）。

## 5. 调试输出与健壮性检查
- 代码中大量使用 `printf("[MODULE] ERROR: ...")` 打印运行时异常（如 DDR 越界、任务指针为空、active_walks 查找失败等），但**没有统一的日志框架或错误级别管理**，属于开发期诊断手段。
- 未发现 `throw`/`std::exception`/`SC_REPORT_ERROR`/`assert`/`panic/recover` 等结构化错误传播机制；关键路径依赖返回值 + goto `stop_and_report_fault` 标签跳转至统一上报点（见 `iommu_translate.cc` 中多处）。