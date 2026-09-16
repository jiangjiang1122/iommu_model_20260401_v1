---
kind: error_handling
name: RISC-V IOMMU 性能模型中的错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/include/iommu_data_structures.hh
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_translate.cc
---

该 SystemC RISC-V IOMMU 性能模型采用**硬件故障队列（Fault Queue, FQ）+ MSI 中断**的架构来统一处理所有错误，遵循 RISC-V IOMMU 规范定义的错误分类与传播方式。核心设计如下：

## 1. 错误类型与编码
- **fault_rec_t** 结构体（`iommu_fault.hh`）定义了 32 字节的故障记录格式，包含 CAUSE(12bit)、PID(20bit)、PV、PRIV、TTYP(6bit)、DID(24bit)、iotval、iotval2 等字段。
- **CAUSE 编码**按规范分为：指令/读写页错（12/13/15）、访客页错（20/21/23）、访问错误（0x01）、数据损坏（0x02）、G-stage 相关错误（0x21~0x23）、以及 256~274 的系统级错误（如 DDT/PDT/MSI PT/MRIF 访问失败、内部数据路径错误等）。
- **TTYP** 标识入站事务类型（未翻译读/写/执行、已翻译读/写/执行、PCIe ATS 请求、消息请求等）。

## 2. 错误报告路径
- **report_fault()**（`iommu_faults.cc`）是统一的错误上报入口，所有模块通过调用此函数将故障写入内存中的 Fault Queue。
- 上报前经过多层过滤：检查 `fqon/fqen` 是否启用、`fqmf/fqof` 是否已置位、DTF（Disable Translation Fault）位控制是否跳过翻译相关错误。
- 写入 FQ 时若遇到内存访问错误或队列溢出，会设置 `fqmf`/`fqof` 并触发 FAULT_QUEUE 中断。

## 3. 中断与通知机制
- **generate_interrupt()**（`iommu_interrupt.cc`）根据 unit 类型（FAULT_QUEUE/PAGE_QUEUE/COMMAND_QUEUE/HPM）设置对应的 pending 位（fip/pip/cip/pmip），并通过 MSI 或 wire interrupt 通知处理器。
- MSI 写入失败时会触发 cause=273（IOMMU MSI write access fault）的故障记录。
- 支持 pending 中断的延迟释放（`release_pending_interrupt()`）。

## 4. 错误传播模式
- **功能模型**（`iommu_fun_model/`）使用 goto 标签跳转到 `page_fault` 或 `stop_and_report_fault` 分支，集中设置 cause 后调用 `report_fault()`。
- **性能模型**（`iommu_perf_model/`）在关键路径使用 `assert()` 断言和 `printf("[MODULE] ERROR: ...")` 打印调试信息，用于验证状态一致性（如 `ptw_outstanding_task_count >= 0` 防止下溢）。
- 测试代码（`ddr/test_ddr.cc`、`rp/test_rp_*.cc`）通过 TLM 响应状态（`TLM_ADDRESS_ERROR_RESPONSE`）和 printf 输出模拟硬件错误场景。

## 5. 设计约束与约定
- 所有硬件相关错误必须通过 `report_fault()` 上报，禁止直接返回错误码。
- 错误记录大小固定为 32 字节（`RVI_IOMMU_FQ_ENTRY_SZ`），便于硬件实现。
- DTF 位允许软件选择性屏蔽翻译阶段错误，但不会抑制非翻译相关的系统错误（如 256~274 类）。
- 性能模型中大量使用 assert 作为运行时不变量检查，而非异常抛出机制。