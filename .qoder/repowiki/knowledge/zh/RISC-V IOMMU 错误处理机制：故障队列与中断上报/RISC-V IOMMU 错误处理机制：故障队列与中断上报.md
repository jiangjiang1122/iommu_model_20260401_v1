---
kind: error_handling
name: RISC-V IOMMU 错误处理机制：故障队列与中断上报
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_interrupt.hh
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/iommu_fun_model/iommu_second_stage_trans.cc
    - iommu/iommu_fun_model/iommu_two_stage_trans.cc
    - iommu/iommu_fun_model/iommu_msi_trans.cc
---

该仓库是一个基于 SystemC 的 RISC-V IOMMU 性能模型，其错误处理完全遵循 RISC-V IOMMU 规范，采用**硬件风格的故障记录 + 内存故障队列 + 中断上报**机制，而非 C++ 异常或返回值错误码。核心设计如下：

### 1. 故障类型与编码
- 所有错误以 `cause`（12位）形式表示，定义在 `iommu_fault.hh` 中，包括：
  - 访问类：`RVI_IOMMU_ACCESS_FAULT`(0x01)、`RVI_IOMMU_DATA_CORRUPTION`(0x02)
  - Guest 阶段：`RVI_IOMMU_GST_PAGE_FAULT`(0x21)、`RVI_IOMMU_GST_ACCESS_FAULT`(0x22)、`RVI_IOMMU_GST_DATA_CORRUPTION`(0x23)
  - 页表/MSI/PDT/DDT 等专用错误（256~274），如 DDT 加载访问失败、MSI PTE 无效、内部数据通路错误等
- 事务类型通过 `TTYP` 字段（6位）标识，区分未翻译/已翻译读执行、PCIe ATS 请求、Message Request 等

### 2. 故障记录结构
- `fault_rec_t` 是 32 字节对齐的内存结构体，包含 CAUSE、PID、PV、PRIV、TTYP、DID、iotval、iotval2 等字段
- 通过 `report_fault()` 统一入口函数生成并写入故障队列

### 3. 故障队列（Fault Queue）机制
- 位于内存中的环形队列，由 `fqb`（基址）、`fqh`（头指针）、`fqt`（尾指针）、`log2szm1`（大小）寄存器控制
- 写入前检查：`fqon`（启用）、`fqen`（使能）、`fqmf`（内存访问故障标志）、`fqof`（溢出标志）
- 队列满时设置 `fqof=1` 并触发 FAULT_QUEUE 中断；写入失败时设置 `fqmf=1`
- 支持 `DTF`（Disable Translation Fault）位过滤：当 DTF=1 时，仅允许特定 cause（256/257/258/259/268/272/273）上报

### 4. 中断系统
- 通过 `generate_interrupt(iommu, unit)` 统一触发，unit 包括 COMMAND_QUEUE、FAULT_QUEUE、HPM、PAGE_QUEUE
- 中断状态由 `ipsr`（interrupt pending status register）管理，支持 `fie`（enable）和 `fip`（pending）位
- MSI 相关错误（如 273）会直接调用 `report_fault` 上报

### 5. 错误传播模式
- 各功能模块（ATC、ATS、Translate、Second Stage、Two Stage、MSI）遇到错误时，使用 `goto stop_and_report_fault` 或直接调用 `report_fault` 跳转至统一错误路径
- 不使用 C++ 异常（无 throw/catch/exception），也不使用返回值错误码
- 调试信息通过 `printf("[MODULE] ...")` 输出到 stdout，便于日志分析

### 6. 关键文件
- `iommu/include/iommu_fault.hh`：故障类型常量、`fault_rec_t` 结构体、`report_fault` 声明
- `iommu/iommu_fun_model/iommu_faults.cc`：`report_fault` 实现，含完整的队列操作逻辑
- `iommu/include/iommu_interrupt.hh`：中断单元枚举、`generate_interrupt` 声明
- `iommu/iommu_fun_model/iommu_ats.cc`、`iommu_translate.cc`、`iommu_second_stage_trans.cc`、`iommu_two_stage_trans.cc`、`iommu_msi_trans.cc`：各模块的错误检测与上报点

### 约定与约束
- 所有故障必须通过 `report_fault` 上报，禁止直接操作寄存器绕过队列
- 故障记录写入失败后，后续故障被丢弃直到软件清除 `fqmf`/`fqof` 位
- DTF 位严格过滤翻译相关故障，但非翻译类错误（如 DDT 损坏、内部错误）始终上报
- 中断触发需检查是否已 pending 且未被 mask（`fip==1 && fie==0`）