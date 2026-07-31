---
kind: error_handling
name: RISC-V IOMMU 仿真平台错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/include/iommu_data_structures.hh
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_top.cc
---

该 RISC-V IOMMU SystemC 仿真平台的错误处理机制严格遵循 RISC-V IOMMU 规范，采用多层错误处理架构：

## 1. TLM-2.0 传输层错误响应
在 SystemC/TLM-2.0 接口层，使用标准的 `tlm::tlm_response_status` 枚举进行错误传播：
- `TLM_OK_RESPONSE`：成功响应
- `TLM_GENERIC_ERROR_RESPONSE`：通用错误
- `TLM_ADDRESS_ERROR_RESPONSE`：地址错误（DDR 测试中使用）
- `TLM_INCOMPLETE_RESPONSE`：未完成响应

各模块通过 `trans.set_response_status()` 设置响应状态，上层通过 `trans.get_response_status()` 检查错误。

## 2. IOMMU 故障队列机制
核心错误处理通过 `report_fault()` 函数实现，将错误信息写入内存中的故障队列（Fault Queue）：
- 故障记录结构 `fault_rec_t` 包含 CAUSE、PID、DID、TTYP、iotval 等字段
- 支持多种故障类型：访问故障（ACCESS_FAULT）、数据损坏（DATA_CORRUPTION）、页故障（PAGE_FAULT）等
- 通过 DTF（Disable Translation Fault）位控制是否报告翻译过程中的故障
- 队列满时设置 fqof 标志并生成中断
- 队列访问错误时设置 fqmf 标志

## 3. 中断系统
错误通过 `generate_interrupt()` 统一处理，支持：
- 故障队列中断（FAULT_QUEUE）
- 页面请求队列中断（PAGE_QUEUE）
- MSI 中断和线中断两种模式
- 中断挂起寄存器（ipsr）和原因到向量映射（icvec）

## 4. 具体错误场景
- **ATC 查找失败**：权限检查失败时跳转到 page_fault 标签，设置相应的 cause 码（指令/读/写页故障）
- **设备上下文查找失败**：返回 IOATC_FAULT 错误码
- **两级地址翻译错误**：S-stage 和 G-stage 翻译过程中产生的各种页故障
- **MSI 写入错误**：MSI 地址访问失败时记录故障

## 5. 错误分类
根据 RISC-V IOMMU 规范，错误分为：
- 访问类错误（CAUSE 0x01, 0x22）
- 数据损坏类错误（CAUSE 0x02, 0x23）
- 页故障类错误（CAUSE 0x21 等）
- 内部路径错误（CAUSE 0x272）
- 自定义错误（CAUSE 2048-4095）

该设计实现了从底层 TLM 传输到高层 IOMMU 规范的完整错误处理链，确保符合 RISC-V IOMMU 规范的行为。