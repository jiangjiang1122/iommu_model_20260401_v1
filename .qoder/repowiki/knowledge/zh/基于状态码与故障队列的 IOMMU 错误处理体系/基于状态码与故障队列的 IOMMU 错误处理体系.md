---
kind: error_handling
name: 基于状态码与故障队列的 IOMMU 错误处理体系
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_interrupt.hh
    - iommu/iommu_fun_model/iommu_interrupt.cc
    - iommu/iommu_fun_model/iommu_second_stage_trans.cc
    - iommu/iommu_fun_model/iommu_process_context.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/iommu_fun_model/iommu_msi_trans.cc
    - iommu/iommu_perf_model/iommu_ref_api.cc
    - iommu/cache_src/subsystem/cache_subsystem.cpp
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/common/json_config.cpp
---

## 1. 总体方法

该仓库是一个基于 SystemC/TLM 的 RISC-V IOMMU 性能模型与验证平台，其错误处理完全遵循 RISC-V IOMMU 规范：不依赖 C++ 异常（仅 JSON 配置解析等辅助模块使用 `std::runtime_error`），而是通过**返回码 + 内存映射寄存器 + 故障队列（Fault Queue）+ MSI 中断**的组合来上报、传播和处理错误。

核心思路：
- 所有底层内存访问统一经过 `read_memory()` / `write_memory()`，返回位标志 `ACCESS_FAULT` / `DATA_CORRUPTION`。
- 上层翻译/上下文/页表遍历函数将这些标志转换为领域错误码（如 `GST_PAGE_FAULT`、`GST_ACCESS_FAULT`、`GST_DATA_CORRUPTION`）并向上返回。
- 最终由 `report_fault()` 把错误编码为 `fault_rec_t` 写入硬件定义的 Fault Queue，并通过 `generate_interrupt()` 触发 MSI 或线中断。
- 缓存子系统（SystemC 部分）仅在违反调用约定时使用 `SC_REPORT_ERROR` 报告严重错误。

## 2. 关键文件与包

| 文件 | 职责 |
|---|---|
| `iommu/include/iommu_fault.hh` | 定义故障记录结构 `fault_rec_t`、TTYP 枚举、CAUSE 常量（`RVI_IOMMU_ACCESS_FAULT`、`RVI_IOMMU_GST_PAGE_FAULT` 等）、`FQ_ENTRY_SZ`，以及 `report_fault()` 声明 |
| `iommu/iommu_fun_model/iommu_faults.cc` | `report_fault()` 实现：检查 FQ 使能/溢出/内存访问故障标志，按 DTF 过滤 CAUSE，构造记录并写入内存，设置 `fqof`/`fqmf` 并触发中断 |
| `iommu/include/iommu_interrupt.hh` | 中断源枚举（`COMMAND_QUEUE`、`FAULT_QUEUE`、`PAGE_QUEUE`、`HPM`）及 `generate_interrupt()` / `release_pending_interrupt()` 声明 |
| `iommu/iommu_fun_model/iommu_interrupt.cc` | `generate_interrupt()`：根据 `ipsr`/各队列 CSR 的 enable/pending 位选择向量，写 MSI 或通过 `msi_pending[]` 延迟；`do_msi()` 中若 MSI 写失败则上报 cause=273 |
| `iommu/iommu_fun_model/iommu_second_stage_trans.cc` | 第二阶段页表遍历：将 `read_memory`/`write_memory` 返回的 `ACCESS_FAULT`/`DATA_CORRUPTION` 映射为 `GST_*` 错误码逐层返回 |
| `iommu/iommu_fun_model/iommu_process_context.cc` | 设备/进程上下文查找：对 PDT entry 加载失败/无效/误配分别返回 cause 265/266/267 |
| `iommu/iommu_perf_model/iommu_ref_api.cc` | 性能模型侧的 `read_memory`/`write_memory` 实现，支持注入 `access_viol_addr`/`data_corruption_addr` 以模拟错误 |
| `iommu/cache_src/subsystem/cache_subsystem.cpp` | 缓存子系统：在 `push_fifo` 非 SystemC 进程调用时通过 `SC_REPORT_ERROR` 报告错误 |
| `iommu/cache_src/common/mini_json.h`、`json_config.cpp` | JSON 配置解析：使用 `throw std::runtime_error(...)` 报告格式/键缺失错误 |

## 3. 架构与约定

### 3.1 错误码分层

| 层级 | 类型 | 说明 |
|---|---|---|
| 物理层 | `ACCESS_FAULT`、`DATA_CORRUPTION` | `read_memory`/`write_memory` 返回的位标志 |
| 阶段层 | `GST_PAGE_FAULT`、`GST_ACCESS_FAULT`、`GST_DATA_CORRUPTION` | 第二阶段页表遍历返回的错误码 |
| 领域层 | 0x01~0x23、256~274 等 CAUSE 值 | 写入 `fault_rec_t.CAUSE`，对应 RISC-V IOMMU 规范的 fault cause |
| 传输层 | TTYP（`RVI_IOMMU_UNTRANSLATED_READ_TRANSACTION` 等） | 标识导致故障的事务类型 |

### 3.2 故障上报流水线

1. 任意翻译路径遇到错误 → 计算 CAUSE → 调用 `report_fault(iommu, cause, iotval, iotval2, TTYP, dtf, device_id, pid_valid, process_id, priv_req)`。
2. `report_fault` 依次检查：
   - `fqcsr.fqon==0 || fqcsr.fqen==0` → 直接丢弃（FQ 未启用）
   - `fqcsr.fqmf==1` → 丢弃（之前已发生 FQ 内存访问故障）
   - `fqcsr.fqof==1` → 丢弃（之前已溢出）
   - DTF 过滤：当 `dtf==1` 且 CAUSE 不在允许列表（256/257/258/259/268/272/273）时丢弃
3. 构造 `fault_rec_t`，计算地址 `fqb*PAGESIZE + fqt*FQ_ENTRY_SZ`，调用 `write_memory` 写入 32 字节记录。
4. 若写入返回 `ACCESS_FAULT`/`DATA_CORRUPTION` → 置 `fqcsr.fqmf=1`；否则 `fqt = (fqt+1) & mask`。
5. 无论成功与否都调用 `generate_interrupt(iommu, FAULT_QUEUE)`。

### 3.3 中断机制

`generate_interrupt()` 针对四种单元（FAULT_QUEUE、PAGE_QUEUE、COMMAND_QUEUE、HPM）分别检查对应的 pending 位（`ipsr.fip/pip/cip/pmip`）和 enable 位（`fqcsr.fie`、`pqcsr.pie`、`cqcsr.cie`），读取 `icvec.*iv` 得到向量，置 pending 位，然后根据 `fctl.wsi` 选择 MSI 或 wire 方式：
- MSI：查 `msi_cfg_tbl[vec]`，若 `msi_vec_ctrl.m==1` 则置 `msi_pending[vec]=1` 延迟发送；否则调用 `do_msi()` 写内存，MSI 写失败时上报 cause=273。
- Wire：由外部 SystemC 拓扑处理（代码中未展开）。

`release_pending_interrupt()` 用于软件清除 pending 后重发被屏蔽的 MSI。

### 3.4 页表遍历中的错误传播

`iommu_second_stage_trans.cc` 中每个 PTE 加载步骤都遵循同一模式：
```cpp
status = read_memory(...);
if (status & ACCESS_FAULT) return GST_ACCESS_FAULT;
if (status & DATA_CORRUPTION) return GST_DATA_CORRUPTION;
```
随后根据 PTE 字段（V/R/W/X/U/N/GADE 等）返回 `GST_PAGE_FAULT`。错误码沿调用栈向上传递，最终由调用方决定是继续页表遍历还是上报 `report_fault`。

### 3.5 缓存子系统的错误处理

缓存子系统位于 `iommu/cache_src/`，独立于 IOMMU 功能模型：
- 使用 SystemC 的 `SC_REPORT_ERROR` 在 `push_fifo` 非 SystemC 进程调用时报错。
- JSON 配置解析使用 `throw std::runtime_error(...)`，属于初始化阶段的快速失败。
- 正常运行时通过 `stats_collector` 收集统计而非抛出异常。

## 4. 约定与约束

- **不使用 C++ 异常进行运行时错误控制**：IOMMU 主路径（翻译、命令队列、中断）全部通过返回值/寄存器状态传递错误，避免异常跨越 SystemC 仿真边界。
- **故障必须进入 Fault Queue 或触发中断**：任何未被 DTF 过滤的故障都会尝试写入 FQ 并触发 FAULT_QUEUE 中断；FQ 满或写失败时通过 `fqof`/`fqmf` 位和中断通知软件。
- **DTF 过滤规则严格遵循规范**：仅 CAUSE 256/257/258/259/268/272/273 可在 DTF=1 时上报，其余翻译相关故障被静默丢弃。
- **MSI 写失败的故障单独上报**：`do_msi()` 中若 MSI 地址访问失败，固定上报 cause=273（IOMMU MSI write access fault），而不是让上层捕获。
- **中断去抖**：每个源的 pending 位（`ipsr.*ip`）一旦置位，同源再次请求会直接返回，直到软件清除。
- **JSON 配置解析快速失败**：`mini_json.h` 与 `json_config.cpp` 在解析失败时抛 `std::runtime_error`，由顶层 `main.cpp` 或测试框架捕获，不属于运行时仿真路径。
- **SystemC 进程约束**：缓存子系统要求 FIFO 操作必须在 SystemC 进程中执行，否则通过 `SC_REPORT_ERROR` 终止当前报告。