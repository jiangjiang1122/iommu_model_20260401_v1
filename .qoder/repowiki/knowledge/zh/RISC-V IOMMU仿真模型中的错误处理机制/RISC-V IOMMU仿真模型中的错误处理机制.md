---
kind: error_handling
name: RISC-V IOMMU仿真模型中的错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_data_structures.hh
    - iommu/include/iommu_interrupt.hh
    - ddr/test_ddr.cc
    - iommu/iommu_top.cc
---

该SystemC RISC-V IOMMU仿真模型实现了完整的硬件级错误处理机制，严格遵循RISC-V特权规范中定义的IOMMU故障报告规范。

**核心架构：**
- 统一的故障记录结构`fault_rec_t`（32字节），包含CAUSE、PID、PV、PRIV、TTYP、DID、iotval、iotval2等字段
- 集中式故障报告函数`report_fault()`位于`iommu_faults.cc`，负责所有故障的标准化处理
- 内存驻留的Fault Queue（FQ）作为硬件队列，通过fqcsr寄存器管理状态

**错误分类体系：**
- 访问类错误：`ACCESS_FAULT`(0x01)、`DATA_CORRUPTION`(0x02)
- 访客页故障：`GST_PAGE_FAULT`(0x21)、`GST_ACCESS_FAULT`(0x22)、`GST_DATA_CORRUPTION`(0x23)
- 事务类型错误：如260号"Transaction type disallowed"
- 内部错误：272号"Internal datapath error"、273号"MSI write access fault"

**DTF控制机制：**
翻译控制寄存器中的`DTF`位控制是否报告地址转换过程中的故障。当DTF=1时，仅允许特定严重错误（256-273范围）被上报，其他翻译相关故障被抑制。

**TLM响应处理：**
- SystemC TLM接口使用标准响应码：`TLM_OK_RESPONSE`、`TLM_ADDRESS_ERROR_RESPONSE`、`TLM_GENERIC_ERROR_RESPONSE`、`TLM_INCOMPLETE_RESPONSE`
- DDR测试模块在地址越界时设置`TLM_ADDRESS_ERROR_RESPONSE`
- 性能模型中广泛使用`set_response_status()`进行错误传播

**中断与队列管理：**
- `generate_interrupt()`统一处理中断生成，支持FAULT_QUEUE等中断源
- 故障队列满时设置`fqof`标志并触发中断
- 队列写入失败时设置`fqmf`标志，后续故障记录被丢弃直到软件清除标志

**权限检查点：**
ATC模块中多处`goto page_fault`跳转处理权限违规，根据访问类型设置不同的cause值（12/13/15分别对应指令/读/写页故障）。