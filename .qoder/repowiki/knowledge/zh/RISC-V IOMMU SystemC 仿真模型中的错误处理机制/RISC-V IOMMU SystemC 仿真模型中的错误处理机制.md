---
kind: error_handling
name: RISC-V IOMMU SystemC 仿真模型中的错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_registers.hh
    - iommu/include/iommu_interrupt.hh
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/iommu_top.cc
    - ddr/test_ddr.cc
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_perf_model/iommu_command_queue.cc
    - iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc
---

## 1. 整体方法

该仓库是一个基于 SystemC/TLM-2.0 的 RISC-V IOMMU 完整仿真模型。错误处理遵循硬件规范（RISC-V IOMMU Spec），采用**“状态码 + 内存驻留队列 + 中断”**的硬件风格，而非 C++ 异常机制。具体分为三层：

- **TLM 传输层错误**：通过 `tlm::tlm_generic_payload` 的 `set_response_status()` / `get_response_status()` 在请求/响应通道上传递错误（`TLM_OK_RESPONSE`、`TLM_GENERIC_ERROR_RESPONSE`、`TLM_ADDRESS_ERROR_RESPONSE`、`TLM_INCOMPLETE_RESPONSE`）。
- **IOMMU 内部故障**：通过 `report_fault()` 将故障记录写入内存中的 Fault Queue（FQ），并触发中断（FAULT_QUEUE）。
- **SystemC 报告**：仅在子系统边界使用 `SC_REPORT_ERROR` 报告违反设计约束的情况（如 FIFO 调用时机错误）。

整个代码库中**没有发现 `throw`/`catch`/`std::exception` 的使用**，也没有 `panic`/`recover` 模式；错误以返回值、状态位和队列事件的形式传播。

## 2. 关键文件与包

| 文件 | 作用 |
|---|---|
| `iommu/include/iommu_fault.hh` | 定义故障类型常量（`ACCESS_FAULT`、`DATA_CORRUPTION`、`GST_PAGE_FAULT` 等）、TTYP 字段编码以及 `fault_rec_t` 32 字节故障记录结构体 |
| `iommu/iommu_fun_model/iommu_faults.cc` | `report_fault()` 实现：检查 FQ 使能、溢出、访问错误，构造记录并通过 `write_memory()` 写入 FQ，设置 `fqmf`/`fqof`，最后 `generate_interrupt(FAULT_QUEUE)` |
| `iommu/include/iommu_registers.hh` | 定义所有 IOMMU 寄存器映射，包括 `fqcsr`（`fqen/fie/fqmf/fqof/fqon/busy`）、`ipsr`（`cip/fip/pmip/pip`）、`icvec`、`tr_response.fault` 等 |
| `iommu/include/iommu_interrupt.hh` | 中断源枚举：`COMMAND_QUEUE`、`FAULT_QUEUE`、`HPM`、`PAGE_QUEUE` |
| `iommu/iommu_fun_model/iommu_interrupt.cc` | `generate_interrupt()` 根据中断源置位 `ipsr` 对应位，若未屏蔽则通过 MSI/Wired 方式发出 |
| `iommu/iommu_top.cc` | TLM 顶层端口：对缺失扩展返回 `TLM_GENERIC_ERROR_RESPONSE`；DDR 响应路径把 `trans.get_response_status() != TLM_OK_RESPONSE` 转为 `rsp.error` 标志 |
| `ddr/test_ddr.cc` | DDR 模型中对越界地址返回 `TLM_ADDRESS_ERROR_RESPONSE` |
| `iommu/cache_src/subsystem/cache_subsystem.cpp` | 唯一使用 `SC_REPORT_ERROR` 的地方，报告“push_fifo must be called from a SystemC process” |
| `iommu/iommu_fun_model/iommu_atc.cc` | ATC TLB 命中后按权限位判断，不满足时跳转到 `page_fault:` 标签设置 cause=12/13/15 并返回 `IOATC_FAULT` |
| `iommu/iommu_fun_model/iommu_ats.cc` | ATS/PRI 相关错误通过 `report_fault()` 上报，并在页请求队列出错时置 `pqmf` |
| `iommu/iommu_perf_model/iommu_command_queue.cc` | 命令队列非法命令、超时、内存访问错误时置 `cqmf/cmd_to/cmd_ill` 并触发 COMMAND_QUEUE 中断 |
| `iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc` | 转发器/命令队列错误路径设置 `TLM_GENERIC_ERROR_RESPONSE` |

## 3. 架构与约定

### 3.1 故障分类与 Cause 编码
- 标准 RISC-V IOMMU 故障 cause：`ACCESS_FAULT`(0x01)、`DATA_CORRUPTION`(0x02)、`GST_PAGE_FAULT`(0x21)、`GST_ACCESS_FAULT`(0x22)、`GST_DATA_CORRUPTION`(0x23)。
- 页表扫描阶段产生的 cause：指令页错(12)、读页错(13)、写/AMO 页错(15)、G-stage 页错(20/21/23)、各类 DDT/PDT/MSI PTE 加载失败(257–263, 265–267)。
- 内部数据通路错误：cause 268/272/273。
- TTYP 字段区分事务来源： untranslated read/write、translated read/write、PCIe ATS translation request、message request。

### 3.2 故障上报流程（`report_fault`）
1. 读取 `fctl.be` 决定端序。
2. 检查 `fqcsr.fqon==0 || fqcsr.fqen==0` → 直接丢弃。
3. 若 `fqcsr.fqmf==1` 或 `fqcsr.fqof==1` → 丢弃后续所有故障。
4. 若 `dtf==1` 且 cause 不在白名单（256/257/258/259/268/272/273）→ 丢弃。
5. 填充 `fault_rec_t`（DID/PID/PV/PRIV/TTYP/iotval/iotval2/CAUSE）。
6. 计算 FQ 地址，若超出物理地址范围 → 置 `ACCESS_FAULT`；否则调用 `write_memory()` 写入 32 字节记录。
7. 若写入返回 `ACCESS_FAULT` 或 `DATA_CORRUPTION` → 置 `fqcsr.fqmf=1`；否则 `fqt = (fqt+1) & mask`。
8. 若 FQ 满（`fqt+1 == fqh`）→ 置 `fqcsr.fqof=1`。
9. 始终调用 `generate_interrupt(FAULT_QUEUE)`。

### 3.3 中断与状态位
- `generate_interrupt(iommu, unit)` 根据 unit 设置 `ipsr.cip/fip/pmip/pip` 对应位。
- 各队列控制寄存器（`cqcsr`、`fqcsr`、`pqcsr`）均提供 `busy` 位，要求软件在写控制位前轮询 busy 为 0。
- 中断向量由 `icvec` 配置，可通过 `msi_cfg_tbl` 的 `msi_vec_ctrl` 的 `mask` 位屏蔽。

### 3.4 TLM 传输错误
- 顶层 `axi_slave_b_transport` 在缺少 `PayloadExtention` 时返回 `TLM_GENERIC_ERROR_RESPONSE`。
- DDR 模型对越界地址返回 `TLM_ADDRESS_ERROR_RESPONSE`。
- 性能模型中大量使用 `TLM_OK_RESPONSE`/`TLM_GENERIC_ERROR_RESPONSE`/`TLM_INCOMPLETE_RESPONSE` 表示成功、通用错误、未完成三种状态。
- 响应回传时通过 `trans.get_response_status() != tlm::TLM_OK_RESPONSE` 转换为 `rsp.error` 布尔值。

### 3.5 SystemC 报告
仅一处使用 `SC_REPORT_ERROR("CacheSubsystem", "push_fifo must be called from a SystemC process")`，用于捕获在非 SystemC 进程中调用 FIFO push 的设计违规。

## 4. 约定与约束

- **不使用 C++ 异常**：全仓未发现 `throw`/`catch`/`std::exception`，错误通过返回值、TLM 状态码、内存队列和中断传递。
- **故障必须经 `report_fault()`**：所有 IOMMU 内部故障统一走此函数，保证 fault record 格式一致、FQ 状态位正确更新。
- **DTF 位可抑制翻译相关故障**：当 `dtf==1` 时，除白名单 cause（256/257/258/259/268/272/273）外的翻译类故障被静默丢弃，但错误响应仍会发回设备。
- **FQ 溢出/访问错误不可恢复**：一旦 `fqof` 或 `fqmf` 置位，后续所有故障记录被丢弃，直到软件写 1 清除对应位。
- **队列控制寄存器写前必须轮询 `busy`**：`ddtp`、`cqcsr`、`fqcsr`、`pqcsr` 的注释明确要求软件在写控制位前确认 `busy==0`。
- **TLM 响应必须显式设置**：所有 `b_transport`/`nb_transport_fw` 路径在返回前必须调用 `set_response_status()`，未设置即视为错误。
- **ATC 权限检查失败跳转至 `page_fault` 标签**：TLB 命中后按 VS_X/VS_R/VS_W/U/SUM 等位判断，不满足则统一跳转到同一故障路径设置 cause=12/13/15。
- **DDR 越界返回地址错误**：测试 DDR 模型对超出内存范围的地址返回 `TLM_ADDRESS_ERROR_RESPONSE`。
- **SystemC 进程约束**：FIFO 操作必须在 SystemC 进程内执行，否则通过 `SC_REPORT_ERROR` 报告。

## 5. 总结

该仓库的错误处理严格遵循 RISC-V IOMMU 规范，采用硬件风格的“内存队列 + 中断 + TLM 状态码”机制，完全避免使用 C++ 异常。核心路径集中在 `iommu_faults.cc` 的 `report_fault()`、`iommu_interrupt.cc` 的中断分发、以及 `iommu_top.cc` 的 TLM 端口错误处理。这种设计使得仿真模型的行为与真实 IOMMU 硬件高度一致，便于与系统级验证环境对接。