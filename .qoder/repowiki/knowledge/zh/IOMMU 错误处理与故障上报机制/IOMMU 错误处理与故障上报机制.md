---
kind: error_handling
name: IOMMU 错误处理与故障上报机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_atc.hh
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_second_stage_trans.cc
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_fun_model/iommu_interrupt.cc
---

该仓库实现了 RISC-V IOMMU 的完整错误处理与故障上报体系，遵循 RISC-V IOMMU 规范，通过统一的 fault_rec_t 结构体和 report_fault() 函数将各类翻译/访问错误写入内存中的 Fault Queue（FQ），并支持 DTF 位控制是否上报翻译相关故障。

**核心机制**
- 统一故障记录：fault_rec_t 联合体定义 32 字节 FQ 条目，包含 CAUSE(12bit)、PID、PV、PRIV、TTYP、DID、iotval、iotval2 等字段
- 集中上报入口：report_fault() 在 iommu_faults.cc 中实现，负责检查 fqon/fqen/fqmf/fqof 状态、DTF 过滤、队列满溢出、内存写入失败等条件
- 多类故障码：ACCESS_FAULT(0x01)、DATA_CORRUPTION(0x02)、GST_PAGE_FAULT(0x21)、GST_ACCESS_FAULT(0x22) 等，以及 256-274 范围的硬件特定原因码
- ATC 返回码：IOATC_MISS/IOATC_HIT/IOATC_FAULT 三态返回值驱动调用方分支

**架构设计**
- 翻译路径错误：iommu_atc.cc 中权限检查失败跳转 page_fault 标签设置 cause=12/13/15，返回 IOATC_FAULT
- 第二阶段翻译：iommu_second_stage_trans.cc 直接返回 GST_PAGE_FAULT/GST_ACCESS_FAULT 给上层
- 页表遍历：iommu_translate.cc 使用 goto stop_and_report_fault 统一跳转到 fault 处理路径
- ATS 请求：iommu_ats.cc 对 Page Request/Stop Marker 错误调用 report_fault()
- MSI/中断：iommu_interrupt.cc 对 MSI 写访问故障上报 cause=273

**DTF 控制策略**
当 tc.DTF=1 时，仅允许 cause∈{256,257,258,259,268,272,273} 的故障上报，其他翻译相关故障被静默丢弃，但设备仍会收到错误响应

**队列管理**
- FQ 满：fqt==(fqh-1) 时设置 fqof 并触发 FAULT_QUEUE 中断
- 队列内存错误：write_memory 返回 ACCESS_FAULT/DATA_CORRUPTION 时设置 fqmf
- 队列禁用：fqon=0 或 fqen=0 时直接丢弃故障记录

**测试层错误处理**
- DDR 模型：set_response_status(TLM_ADDRESS_ERROR_RESPONSE) 模拟地址错误
- 性能模型：printf 输出 ERROR 日志 + assert 断言关键不变式（如 ptw_outstanding_task_count>=0）
- 验证脚本：大量 printf("[TEST] ERROR: ...") 用于测试用例失败标记