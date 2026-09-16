---
kind: error_handling
name: RISC-V IOMMU模型错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/include/iommu_req_rsp.hh
    - iommu/include/iommu_task.hh
---

该RISC-V IOMMU SystemC性能模型采用硬件规范驱动的错误处理架构，严格遵循RISC-V特权规范和IOMMU规范，通过fault队列、中断机制和状态码三位一体的方式处理各类错误。

核心错误类型与定义：故障原因代码(CAUSE)在iommu_fault.hh中统一定义，包括基础故障(ACCESS_FAULT、DATA_CORRUPTION)、Guest页表故障(GST_PAGE_FAULT等)和特殊故障(256-273范围)。事务类型(TTYP)编码入站事务来源，如未翻译读执行、PCIe ATS请求、消息请求等。

错误传播机制：report_fault函数实现统一故障上报逻辑，检查fault队列使能和错误状态，根据DTF位过滤特定类型的故障，填充fault记录并写入内存队列，处理队列溢出和内存访问故障，触发FAULT_QUEUE中断。

响应状态系统：status_t枚举定义三种完成状态(SUCCESS、UNSUPPORTED_REQUEST、COMPLETER_ABORT)，通过iommu_to_hb_rsp_t结构体返回给主机桥，并在寄存器tr_response.fault中反映。

中断处理机制：generate_interrupt函数支持四种中断源(FAULT_QUEUE、PAGE_QUEUE、COMMAND_QUEUE、HPM)，通过MSI或有线中断两种方式生成，当MSI被屏蔽时中断进入msi_pending队列延迟处理。

DDR响应中的error字段标记传输错误，ddr_rsp_entry_t结构体包含任务ID、数据长度、错误标志和时间戳用于性能统计。各模块通过goto stop_and_report_fault标签集中处理地址翻译过程中的错误。