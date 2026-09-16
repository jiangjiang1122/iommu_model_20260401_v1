---
kind: error_handling
name: 基于 fault queue 与寄存器状态位的 IOMMU 错误处理机制
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_req_rsp.hh
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/include/iommu_registers.hh
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/cache/pt_cache.cpp
    - iommu/cache_src/cache/walker_cache.cpp
    - iommu/cache_src/cache/dedup_cache.cpp
    - iommu/cache_src/replacement/plru_policy.cpp
    - iommu/cache_src/replacement/srrip_policy.cpp
    - iommu/iommu_perf_model/iommu_perf_ptw.cc
    - iommu/iommu_top.cc
---

## 1. 整体方法

该仓库是一个基于 SystemC 的 RISC-V IOMMU 性能/功能模型，**没有使用 C++ 异常（throw/catch）作为运行时错误传播机制**。错误处理遵循硬件规范（RISC-V IOMMU spec），通过以下三种手段组合实现：
- **内存映射寄存器位**：如 `fqcsr.fqof`（fault-queue overflow）、`fqcsr.fqmf`（memory access fault）、`ipsr` 中断状态等，用于向软件报告故障。
- **Fault Queue（FQ）**：通过 `report_fault()` 将 32 字节 `fault_rec_t` 写入内存中的 fault queue，并触发中断。
- **返回值 + goto 标签**：在翻译流程中用 `goto stop_and_report_fault` 集中上报，调用方根据 `status_t`（SUCCESS / UNSUPPORTED_REQUEST / COMPLETER_ABORT）判断成功或失败。

此外，cache 子系统、replacement policy、JSON 配置加载等辅助模块大量使用 `assert(...)` 做参数/不变量校验；配置加载失败时抛 `std::runtime_error`。

## 2. 关键文件与位置

| 文件 | 作用 |
|---|---|
| `iommu/include/iommu_fault.hh` | 定义 `fault_rec_t` 联合体、TTYP 枚举常量、`report_fault()` 声明 |
| `iommu/iommu_fun_model/iommu_faults.cc` | `report_fault()` 实现：检查 FQ 使能/溢出/内存访问错误，写队列，置 `fqof`/`fqmf`，触发 FAULT_QUEUE 中断 |
| `iommu/include/iommu_req_rsp.hh` | 定义 `status_t`（SUCCESS=0, UNSUPPORTED_REQUEST=1, COMPLETER_ABORT=4）及请求/响应结构体 |
| `iommu/iommu_fun_model/iommu_translate.cc` | 翻译主流程：遇到非法模式/设备 ID 宽度/上下文缺失等，设置 `cause` 后 `goto stop_and_report_fault`，最终填充 `rsp_msg->status` |
| `iommu/include/iommu_registers.hh` | 所有寄存器偏移/字段宏（`FQB_OFFSET`、`FQH_OFFSET`、`FQT_OFFSET`、`FQCSR_OFFSET`、`IPSUR_OFFSET` 等），是错误状态上报的载体 |
| `iommu/cache_src/common/json_config.cpp` | 唯一显式 `throw std::runtime_error` 的位置（配置文件打开失败） |
| `iommu/cache_src/cache/*.cpp`、`replacement/*.cpp` | 大量 `assert(...)` 校验缓存参数（num_rams/power-of-2、set < num_sets_、way < num_ways_ 等） |
| `iommu/iommu_perf_model/iommu_perf_ptw.cc` | 使用 `assert(ptw_outstanding_task_count >= 0)` 防止下溢 |
| `iommu/iommu_top.cc` | reset 路径中 `reset_iommu()` 返回 `<0` 时用 `printf` 打印行号；AHB 读写路径统一返回 `TLM_OK_RESPONSE`，b_transport 兼容路径返回 `TLM_GENERIC_ERROR_RESPONSE` |

## 3. 架构与约定

### 3.1 Fault 上报流水线
`iommu_translate_*` → 计算 `cause` → `report_fault(iommu, cause, iotval, iotval2, TTYP, dtf, ...)` → 检查 `fqon/fqen/fqmf/fqof` → 构造 `fault_rec_t` → `write_memory()` 写入 FQ → 若写失败置 `fqmf` → 更新 `fqt` → `generate_interrupt(FAULT_QUEUE)`。
DTF（disable translation fault）位会过滤掉部分地址转换类 fault（见 `iommu_faults.cc` 第 86-90 行的 cause 白名单）。

### 3.2 翻译流程的错误分支
`iommu_translate.cc` 采用“步骤化”风格：每个阶段遇到非法条件即设置 `cause` 并 `goto stop_and_report_fault`。成功路径则继续到 step_20 并最终填充 `iommu_to_hb_rsp_t` 的 `status` 字段。

### 3.3 返回值约定
- 翻译函数不抛异常，而是通过输出参数 `iommu_to_hb_rsp_t *rsp_msg` 的 `status` 字段回传结果。
- 低层 `write_memory()` 返回包含 `ACCESS_FAULT` / `DATA_CORRUPTION` 标志的状态字，由 `report_fault` 直接解读。

### 3.4 assert 策略
Cache 和 replacement 模块在构造函数/访问入口使用 `assert(... && "...")` 强制约束：
- `num_rams_` 为 2 的幂且整除 `num_sets_`
- `num_sets_` 为 2 的幂
- `set < num_sets_`、`way < num_ways_`
- `m_bits >= 1 && m_bits <= 8`
这些断言在 release 构建中被移除，属于开发期不变量保护。

### 3.5 JSON 配置错误
`json_config.cpp` 在无法打开配置文件时 `throw std::runtime_error("Cannot open config file: " + json_path)`，这是整个代码库中唯一一处抛出标准异常的点。

## 4. 约定与约束

- **禁止使用 C++ 异常进行正常控制流**：核心 IOMMU 路径（translate、fault、reg 访问）完全不使用 try/catch，仅依赖寄存器位和返回值。
- **Fault 必须经 `report_fault()` 上报**：所有违反 IOMMU 规范的情形（Off/Bare 模式误用、DDT/PDT 访问失败、G-stage page fault 等）都通过统一的 `cause` 编码上报，而非就地退出。
- **FQ 满/访存错误不阻塞**：当 `fqof==1` 或 `fqmf==1` 时直接丢弃 fault 记录并返回，避免死锁。
- **断言用于设计期不变量**：cache/replacement/PTW 计数器的边界检查全部用 `assert`，不在运行时代码路径中返回错误码。
- **SystemC 顶层错误以 printf + TLM response status 表达**：`before_end_of_elaboration` 中 reset 失败打印 ANSI 彩色行号；非性能路径的 b_transport 返回 `TLM_GENERIC_ERROR_RESPONSE`。
- **无全局 errno/错误码表**：错误语义完全由 IOMMU spec 的 cause 值（如 256=all inbound disallowed、260=transaction type disallowed、0x21=gst page fault 等）承载。