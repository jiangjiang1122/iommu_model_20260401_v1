---
kind: error_handling
name: RISC-V IOMMU SystemC 性能模型错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_req_rsp.hh
    - iommu/include/iommu_task.hh
    - ddr/test_ddr.cc
---

该 RISC-V IOMMU SystemC 性能模型项目的错误处理采用分层策略，结合硬件规范驱动的错误报告、TLM 响应状态和断言验证三种方式：

**1. 硬件级错误报告（Fault Queue）**
- `iommu_fault.hh` 定义了完整的故障记录结构 `fault_rec_t`，包含 CAUSE、PID、PV、PRIV、TTYP、DID、iotval、iotval2 等字段
- `report_fault()` 函数实现符合 RISC-V IOMMU 规范的故障队列写入逻辑，支持访问故障、数据损坏、页故障等多种 CAUSE 类型
- 通过 `fqcsr` 寄存器管理故障队列状态（fqmf、fqof），当队列满或内存访问失败时设置相应标志位并触发中断
- DTF（disable-translation-fault）位控制是否报告地址转换过程中的故障

**2. TLM 传输层错误传播**
- `iommu_req_rsp.hh` 定义 `status_t` 枚举，包含 SUCCESS、UNSUPPORTED_REQUEST、COMPLETER_ABORT 等标准 PCIe 完成状态
- DDR 模型在 `test_ddr.cc` 中使用 `trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE)` 返回地址越界错误
- 请求/响应结构体中通过 `error` 字段传递 DDR 响应错误状态

**3. 运行时断言验证**
- 缓存子系统广泛使用 `assert()` 验证配置参数（如 `num_rams_` 必须是 2 的幂）、索引边界（`set < num_sets_`）等不变式
- 替换策略模块对 way/set 索引进行断言检查
- JSON 配置文件加载失败时抛出 `std::runtime_error` 异常

**4. 任务级错误标记**
- `iommu_task_t` 结构体中的 `cause`、`iotval`、`iotval2` 字段用于记录翻译失败的详细信息
- `task_state_t` 包含 `TASK_FAULT` 状态用于错误处理流程控制
- `ddr_rsp_entry_t` 中的 `error` 布尔字段标识 DDR 响应错误

**5. 错误分类与处理**
- 区分设备侧错误（通过 TLM 响应状态返回）和内部错误（通过 fault queue 报告）
- 支持 Guest Page Fault、Access Fault、Data Corruption 等多层次错误类型
- 错误处理遵循 RISC-V IOMMU 规范，确保与硬件行为一致