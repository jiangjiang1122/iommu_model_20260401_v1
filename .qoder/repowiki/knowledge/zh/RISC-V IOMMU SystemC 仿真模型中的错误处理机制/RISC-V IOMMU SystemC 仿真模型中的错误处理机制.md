---
kind: error_handling
name: RISC-V IOMMU SystemC 仿真模型中的错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_data_structures.hh
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/include/iommu_atc.hh
    - ddr/test_ddr.cc
---

## 1. 采用的系统/方法

该仓库是一个基于 SystemC/TLM-2.0 的 RISC-V IOMMU 周期精确仿真模型，**不使用 C++ 异常（throw/catch）**。错误通过以下三种互补机制处理：

1. **TLM-2.0 响应状态**：在 TLM 接口层使用 `trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE / TLM_OK_RESPONSE)` 返回错误。
2. **硬件故障队列（Fault Queue）**：遵循 RISC-V IOMMU 规范，通过 `report_fault()` 将 fault record 写入内存中的 fault queue，并触发中断。
3. **返回值 + goto 标签**：在 ATC/翻译路径中用 `goto page_fault` 集中设置 cause 码并返回 `IOATC_FAULT` 等枚举值。

## 2. 关键文件与包

| 文件 | 作用 |
|---|---|
| `iommu/include/iommu_fault.hh` | 定义 fault record (`fault_rec_t`)、TTYP/CAUSE 常量（如 `ACCESS_FAULT=0x01`、`GST_PAGE_FAULT=0x21`）、`report_fault()` 声明 |
| `iommu/iommu_fun_model/iommu_faults.cc` | `report_fault()` 实现：检查 FQ 使能、DTF 过滤、溢出/访问错误、写入内存并推进 fqt、触发 FAULT_QUEUE 中断 |
| `iommu/include/iommu_data_structures.hh` | 定义 `tc_t` 控制寄存器（含 `DTF`、`SADE`、`GADE`、`SXL` 等位域），以及 device_context_t、process_context_t 等结构体 |
| `iommu/iommu_fun_model/iommu_atc.cc` | ATC 权限检查，命中失败时 `goto page_fault` 设置 cause=12/13/15（指令页错/读页错/写页错）并返回 `IOATC_FAULT` |
| `iommu/iommu_fun_model/iommu_ats.cc` | ATS 请求处理，遇到非法事务类型或禁止的事务时调用 `report_fault()` 上报 cause=256/260 |
| `ddr/test_ddr.cc` | DDR 模型中越界访问通过 `TLM_ADDRESS_ERROR_RESPONSE` 返回错误 |
| `iommu/include/iommu_atc.hh` | 定义 `IOATC_FAULT` 宏（值为 `RVI_IOMMU_IOATC_FAULT=2`） |

## 3. 架构与约定

### 3.1 Fault Record 格式
`fault_rec_t` 是 128-bit（4×64bit）联合体，字段包括 CAUSE(12bit)、PID(20bit)、PV、PRIV、TTYP(6bit)、DID(24bit)、iotval、iotval2，严格对齐 RISC-V IOMMU 规范的 fault-queue entry 布局。

### 3.2 DTF（Disable Translation Fault）过滤
`report_fault()` 在写入前根据 `dtf` 参数过滤：当 DTF=1 时仅允许 cause ∈ {256, 257, 258, 259, 268, 272, 273}（即“所有入站事务被禁止”、“DDT/PDT/MSI PT/MRIF 访问错误/无效/损坏”、“内部数据路径错误”、“IOMMU MSI 写访问错误”），其余翻译相关 fault（页错、访问错等）被静默丢弃。

### 3.3 Fault Queue 管理
- 写入前检查 `fqcsr.fqon/fqen`（必须同时为 1）、`fqmf`（memory access fault 标志）、`fqof`（overflow 标志）；任一条件不满足则直接 return。
- 满队时置 `fqof=1` 并触发中断，丢弃记录。
- 写入 fault queue 内存时使用 `write_memory()`，若返回 ACCESS_FAULT/DATA_CORRUPTION 则置 `fqmf=1`。
- 成功写入后推进 `fqt` 索引并触发 `FAULT_QUEUE` 中断。

### 3.4 翻译路径错误传播
ATC/ATS 模块检测到权限违规、未映射、事务类型不允许等情况时，统一通过 `goto page_fault` 或赋值 `cause = ...` 后调用 `report_fault()`，并以 `IOATC_FAULT` 等枚举值返回给上层调度器，而不是抛出异常。

## 4. 约定与约束

- **不使用 C++ 异常**：全仓搜索未发现 `throw`/`catch`/`std::exception`，错误一律通过返回值、TLM 响应状态和 fault queue 传递。
- **Fault cause 编码遵循 RISC-V IOMMU 规范**：页错使用 12/13/15，Guest 页错使用 20/21/23，设备级错误使用 256~274 范围。
- **DTF 位是强制过滤器**：任何翻译相关 fault 在写入 fault queue 前必须经过 DTF 判断，这是规范行为而非可选优化。
- **Fault queue 不可重入**：一旦 `fqmf` 或 `fqof` 置位，后续所有 fault 均被丢弃，直到软件清零对应位。
- **TLM 层错误使用标准响应码**：DDR 等子系统对越界地址返回 `TLM_ADDRESS_ERROR_RESPONSE`，符合 TLM-2.0 约定。
- **中断与 fault 解耦**：fault 记录写入完成后统一调用 `generate_interrupt(iommu, FAULT_QUEUE)`，由中断控制器决定是否向主机暴露。