---
kind: error_handling
name: RISC-V IOMMU SystemC 模型错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_req_rsp.hh
    - iommu/include/iommu_dedup_params.hh
    - iommu/include/iommu_task.hh
---

该 RISC-V IOMMU SystemC 性能模型采用**结构化状态码 + 硬件故障队列**的错误处理架构，而非 C++ 异常机制。错误处理分为三个层次：

## 1. 传输层错误（TLM 响应）
- 使用 `status_t` 枚举定义标准 PCIe 完成状态：`SUCCESS`、`UNSUPPORTED_REQUEST`、`COMPLETER_ABORT`
- DDR 模拟通过 `tlm::tlm_generic_payload::set_response_status(TLM_ADDRESS_ERROR_RESPONSE)` 返回地址错误
- `ddr_rsp_entry_t.error` 字段标记 DDR 响应错误，在任务处理中传播

## 2. 翻译阶段错误（PTE 状态）
- `PTEStatus` 枚举定义 PTE 有效性：`PTE_VALID`、`PTE_INVALID`、`PTE_ERROR`、`PTE_RESERVED`
- 权限检查失败时设置 `check_access_perms` 标志位，由 PTW 路径生成相应故障
- 地址越界、未对齐等错误通过 `assert()` 断言捕获（开发期检测）

## 3. 硬件级故障记录（Fault Queue）
核心机制是 `report_fault()` 函数实现的 RISC-V IOMMU 规范故障队列：
- `fault_rec_t` 结构体包含 CAUSE、PID、TTYP、DID、iotval 等字段
- 支持多种故障类型：访问故障（ACCESS_FAULT）、数据损坏（DATA_CORRUPTION）、G-stage 页故障（GST_PAGE_FAULT）等
- 通过 `fqcsr` 寄存器管理队列状态：`fqon`（启用）、`fqmf`（内存访问故障）、`fqof`（溢出）
- `DTF` 位控制是否报告翻译相关故障，但不影响非翻译相关的错误响应

## 4. 断言与配置验证
- 缓存模块广泛使用 `assert()` 验证参数合法性（如 `num_rams_ must be power of 2`）
- JSON 配置文件加载失败时抛出 `std::runtime_error` 异常
- 性能模型中使用 `assert(ptw_outstanding_task_count >= 0)` 防止计数器下溢

## 5. 错误传播路径
- 任务级：`iommu_task_t.cause`、`iotval`、`iotval2` 字段携带故障信息
- 流水线状态：`task_state_t::TASK_FAULT` 标记故障处理阶段
- 设备上下文：`device_context_t.tc.DTF` 控制故障报告行为

该设计严格遵循 RISC-V IOMMU 规范，将软件可恢复错误（如 PTE 无效）与硬件不可恢复错误（如数据损坏）区分处理，并通过标准化的故障队列向操作系统提供诊断信息。