---
kind: error_handling
name: RISC-V IOMMU 仿真模型中的错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_top.cc
    - ddr/test_ddr.cc
    - iommu/include/iommu_data_structures.hh
---

该仓库是一个基于 SystemC 的 RISC-V IOMMU 完整仿真模型，其错误处理遵循 RISC-V IOMMU 规范，主要采用**故障队列（Fault Queue）+ TLM 响应状态 + 中断上报**的组合方式。核心设计如下：

### 1. 故障记录与上报（Fault Queue）
- **fault_rec_t** 结构体定义在 `iommu/include/iommu_fault.hh`，包含 CAUSE、PID、PV、PRIV、TTYP、DID、iotval、iotval2 等字段，每个记录 32 字节。
- **report_fault()** 函数（`iommu/iommu_fun_model/iommu_faults.cc`）是统一的故障上报入口，负责将故障记录写入内存中的 fault queue，并触发 FAULT_QUEUE 中断。
- 支持 DTF（disable-translation-fault）位控制是否报告地址转换相关的故障，但不会禁用对设备的错误响应。
- 故障队列满时设置 fqof 位并触发中断；写入失败时设置 fqmf 位。

### 2. 故障原因编码（CAUSE）
- 使用数字编码表示不同类型的故障，如：
  - 12/13/15：指令页故障/读页故障/写页故障
  - 20/21/23：Guest 页故障相关
  - 256-274：IOMMU 内部错误（DDT/PDT 访问错误、数据损坏等）
- TTYP 字段标识入站事务类型（未翻译读/写、ATS 请求、消息请求等）

### 3. TLM 响应状态传播
- 通过 SystemC TLM 的 `set_response_status()` 和 `get_response_status()` 传递错误状态
- 使用标准状态：`TLM_OK_RESPONSE`、`TLM_GENERIC_ERROR_RESPONSE`、`TLM_ADDRESS_ERROR_RESPONSE`、`TLM_INCOMPLETE_RESPONSE`
- 在 `ddr/test_ddr.cc` 中，DDR 访问越界时返回 `TLM_ADDRESS_ERROR_RESPONSE`
- 在 `iommu_top.cc` 中，根据翻译结果设置相应的响应状态

### 4. 中断机制
- `generate_interrupt()` 函数（`iommu/iommu_fun_model/iommu_interrupt.cc`）统一处理中断生成
- 支持 FAULT_QUEUE、PAGE_QUEUE 等中断源
- 中断可通过 MSI 或 wire-based 方式通知处理器

### 5. 页面权限检查与跳转式错误处理
- 在 `iommu_atc.cc` 中使用 `goto page_fault` 模式进行权限检查
- 根据执行/读/写操作和特权级别设置不同的 cause 值

### 6. 性能模型中的错误分类
- `iommu_perf_model.hh` 中定义了 guest fault cause 辅助函数，将基础故障码映射到具体的任务 cause
- 支持区分指令/加载/存储类型的 guest-page fault

### 架构约定
- 所有故障最终都通过 `report_fault()` 统一上报，确保一致性
- 错误处理与正常路径分离，通过 goto 标签跳转到错误处理逻辑
- 硬件寄存器状态（fqcsr、ipsr 等）与软件可观察的错误状态保持同步
- 测试代码通过 TLM 响应状态验证错误路径的正确性