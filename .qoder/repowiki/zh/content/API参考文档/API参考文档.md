# API参考文档

<cite>
**本文档引用的文件**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)
- [iommu_utils.hh](file://iommu/include/iommu_utils.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_perf_model.hh](file://iommu/include/iommu_perf_model.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)
</cite>

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能考量](#性能考量)
8. [故障排查指南](#故障排查指南)
9. [结论](#结论)
10. [附录](#附录)

## 简介
本文件为RISC-V IOMMU的完整API参考文档，覆盖以下主题：
- 核心数据结构：iommu_task_t任务数据结构、设备/进程上下文结构、页表项结构等字段定义、数据类型与使用规则
- 寄存器接口：寄存器定义、访问权限控制、状态查询方法
- 请求/响应接口：消息格式、协议规范与数据传输机制
- 接口规范：函数签名、参数说明、返回值定义与异常处理
- 实际使用模式与示例（以路径形式给出）
- 版本兼容性、废弃功能迁移指南与性能优化建议

## 项目结构
该仓库采用按功能域分层的组织方式：
- include：对外公开的头文件，定义数据结构、寄存器、接口声明
- iommu_fun_model：功能模型实现（翻译、上下文定位、中断、MSI等）
- iommu_perf_model：性能模型与寄存器访问实现
- iommu：顶层模块与测试用例

```mermaid
graph TB
subgraph "公共头文件"
A["iommu_task.hh"]
B["iommu_data_structures.hh"]
C["iommu_struct.hh"]
D["iommu_registers.hh"]
E["iommu_req_rsp.hh"]
F["iommu_fault.hh"]
G["iommu_translate.hh"]
H["iommu_interrupt.hh"]
I["iommu_command_queue.hh"]
J["iommu_ref_api.hh"]
K["iommu_perf_model.hh"]
end
subgraph "功能模型"
M["iommu_translate.cc"]
N["iommu_process_context.cc"]
end
subgraph "性能模型"
O["iommu_reg.cc"]
end
A --> M
B --> M
C --> M
D --> O
E --> M
F --> M
G --> M
H --> M
I --> M
J --> M
K --> M
M --> O
```

**图表来源**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_perf_model.hh](file://iommu/include/iommu_perf_model.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

**章节来源**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_perf_model.hh](file://iommu/include/iommu_perf_model.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

## 核心组件
本节聚焦于IOMMU的关键数据结构与接口，帮助快速建立对系统的整体认知。

- 任务状态机与行走阶段
  - 任务状态枚举：任务解析、上下文查询、TLB/PTW/MSIPTW行走、转发、故障、完成等
  - 行走类型：DDT/PDT/VS-PT/G-PT/MSI-PT
  - xDTW/PTW/MSIPTW行走阶段：非叶子、读DC/PC、隐式GS等
  - 参考路径：[任务状态与行走阶段定义](file://iommu/include/iommu_task.hh)

- 任务上下文与流水线
  - iommu_task_t：包含设备/进程标识、分类字段、上下文、翻译结果、管道状态、行走上下文等
  - 收集器条目、DDR请求/响应/挂起队列条目、控制路径DDR请求等
  - 参考路径：[任务与收集器结构](file://iommu/include/iommu_task.hh)

- 设备/进程上下文与页表控制
  - 设备上下文：tc/iohgatp/ta/fsc/msi相关字段
  - 进程上下文：ta/fsc
  - 页表指针：iosatp/iohgatp/pdtp/msiptp及掩码/模式
  - 参考路径：[设备/进程上下文与页表控制](file://iommu/include/iommu_data_structures.hh)

- 寄存器与寄存器文件
  - 能力寄存器、特性控制寄存器、DDTP、命令/故障/页面请求队列基址与指针、CSR、中断状态、性能计数器等
  - 参考路径：[寄存器定义与布局](file://iommu/include/iommu_registers.hh)

- 请求/响应消息格式
  - 主桥到IOMMU请求：设备ID、进程ID、读写/AMO/执行/特权/是否CXL等
  - 翻译响应：PPN/S/N/Priv/U/R/W/Exe/PBMT/MSI/MRIF等
  - 参考路径：[请求/响应接口](file://iommu/include/iommu_req_rsp.hh)

- 故障记录与报告
  - 故障类型编码、故障队列记录字段、报告接口
  - 参考路径：[故障接口](file://iommu/include/iommu_fault.hh)

- 翻译与地址转换
  - PTE结构（S/VS/G/MSI）、定位设备/进程上下文、两阶段/第二阶段/MSI地址翻译
  - 参考路径：[翻译接口](file://iommu/include/iommu_translate.hh)

- 中断与命令队列
  - 中断源类型、生成/释放中断、命令类型（IOFENCE/IOTINVAL/IODIR/ATS等）与字段
  - 参考路径：[中断与命令队列](file://iommu/include/iommu_interrupt.hh)、[命令队列](file://iommu/include/iommu_command_queue.hh)

- 性能模型辅助
  - 事务类型分类、DDI提取、VPN提取、规范地址检查、裸模式页大小、MSI地址判定、故障原因设置等
  - 参考路径：[性能模型辅助](file://iommu/include/iommu_perf_model.hh)

**章节来源**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_perf_model.hh](file://iommu/include/iommu_perf_model.hh)

## 架构总览
IOMMU通过内存映射寄存器接口对外暴露能力与配置；功能模型负责请求解析、上下文定位、地址翻译与故障上报；性能模型负责寄存器访问与事件统计。

```mermaid
graph TB
HB["主桥/设备"]
REG["寄存器文件<br/>capabilities/fctl/ddtp/cq/fq/pq/CSR/HPM"]
FM["功能模型<br/>翻译/上下文/中断/MSI"]
PM["性能模型<br/>寄存器访问/事件统计"]
MEM["系统内存/DDR"]
HB --> |"请求/响应"| FM
FM --> |"读写/隐式访问"| MEM
FM --> |"故障/中断"| REG
REG --> |"读/写"| PM
PM --> |"寄存器值/计数"| HB
```

**图表来源**
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

## 详细组件分析

### 任务数据结构与状态机
- iommu_task_t字段族
  - 身份字段：task_id/device_id/process_id/pid_valid/iova/length/at/exec_req/priv_req/no_write/is_cxl_dev/read_writeAMO/timestamp
  - 分类字段：TTYP/is_read/is_write/is_exec/priv/SUM/DDI
  - 上下文字段：DC/PC/iosatp/iohgatp/PSCV/GV/PSCID/GSCID/DID/PID/PV/DTF/check_access_perms/SXL/SADE/GADE
  - 翻译结果：pa/gpa/page_sz/gst_page_sz/vs_pte/g_pte/is_msi/is_mrif/mrif_nid/dest_mrif_addr/cause/iotval/iotval2/is_bare_translation/is_b_transport
  - 流水线状态：state/dc_valid/dc_hit/pc_valid/pc_hit/need_pc/tlm_trans_ptr
  - 行走上下文：walk_ctx（含walk_type/walk_phase/level/max_levels/base_addr/indexes/read_*等）
- 状态机与行走阶段
  - 任务状态涵盖解析、DC/PC查询/命中/缺失、TLB/PTW/MSIPTW、转发、故障、完成
  - 行走上下文支持多级页表索引、中间PPN缓存、AD位更新上下文、DDR读计数
- 使用规则
  - 任务生命周期由状态机驱动，各阶段需满足上下文有效性与页表一致性
  - 行走上下文在PTW中用于缓存中间结果，减少重复访问

```mermaid
stateDiagram-v2
[*] --> 初始化
初始化 --> 解析中 : "接收请求"
解析中 --> 解析完成 : "分类/属性提取"
解析完成 --> 设备上下文查询 : "定位DC"
设备上下文查询 --> 设备上下文命中 : "命中缓存"
设备上下文查询 --> 设备上下文缺失 : "未命中/读内存"
设备上下文命中 --> 进程上下文查询 : "定位PC"
设备上下文缺失 --> 进程上下文查询
进程上下文查询 --> 进程上下文命中 : "命中缓存"
进程上下文查询 --> 进程上下文缺失 : "未命中/读内存"
进程上下文命中 --> TLB查询 : "两阶段翻译"
进程上下文缺失 --> TLB查询
TLB查询 --> TLB命中 : "命中"
TLB查询 --> TLB缺失 : "缺失/PTW"
TLB缺失 --> PTW请求 : "发起页表遍历"
PTW请求 --> PTW完成 : "获得PTE/页大小"
PTW完成 --> 转发 : "构造响应"
TLB命中 --> 转发
转发 --> 完成 : "发送响应"
解析完成 --> 故障 : "非法/不支持"
设备上下文缺失 --> 故障
进程上下文缺失 --> 故障
PTW请求 --> 故障 : "页表故障"
转发 --> 故障 : "访问违规"
故障 --> 完成
```

**图表来源**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

**章节来源**
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

### 设备/进程上下文与页表控制
- 设备上下文（DC）
  - tc：控制位（EN_ATS/EN_PRI/T2GPA/DTF/PDTV/PRPR/GADE/SADE/DPE/SBE/SXL/custom等）
  - iohgatp：G-stage根页表与GSCID
  - ta：PSCID/rcid/mcid
  - fsc：iosatp或pdtp（Bare/目录表）
  - msi扩展：msiptp/msi_addr_mask/msi_addr_pattern/reserved
- 进程上下文（PC）
  - ta：V/ENS/SUM/PSCID/reserved
  - fsc：iosatp（VS-stage根页表）
- 页表指针与模式
  - iosatp/iovsatp：S/VS-stage模式
  - iohgatp：G-stage模式
  - pdtp：目录表模式（PD8/PD17/PD20/Bare）
  - msiptp：MSI页表模式（Off/Flat）

```mermaid
classDiagram
class 设备上下文_DC {
+tc_t tc
+iohgatp_t iohgatp
+ta_t ta
+fsc_t fsc
+msiptp_t msiptp
+msi_addr_mask_t msi_addr_mask
+msi_addr_pattern_t msi_addr_pattern
+uint64_t reserved
}
class 进程上下文_PC {
+pc_ta_t ta
+pc_fsc_t fsc
}
class 页表指针_iosatp {
+PPN : uint64_t
+MODE : uint64_t
}
class 页表指针_iohgatp {
+PPN : uint64_t
+GSCID : uint64_t
+MODE : uint64_t
}
class 页表指针_pdtp {
+PPN : uint64_t
+MODE : uint64_t
}
设备上下文_DC --> 页表指针_iosatp : "fsc.iosatp"
设备上下文_DC --> 页表指针_iohgatp : "iohgatp"
设备上下文_DC --> 页表指针_pdtp : "fsc.pdtp"
进程上下文_PC --> 页表指针_iosatp : "fsc.iosatp"
```

**图表来源**
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)

**章节来源**
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)

### 寄存器接口与访问控制
- 寄存器布局与偏移
  - 能力寄存器（只读）、特性控制寄存器（fctl）、DDTP（设备目录表指针）
  - 命令/故障/页面请求队列基址与指针、CSR、中断状态、HPM计数器与事件选择器
- 访问权限与对齐约束
  - 仅支持4字节与8字节访问；偏移必须按访问宽度对齐；不可越界或跨寄存器访问
  - DDTP写入可能触发异步操作，busy位指示忙状态
- 状态查询
  - 通过cqcsr/fqcsr/pqcsr查询队列启用、中断使能、内存故障、超时、非法命令等
  - 通过ipsr查询中断挂起状态
  - 通过iocntovf汇总HPM溢出状态

```mermaid
flowchart TD
Start(["寄存器访问入口"]) --> CheckAlign["校验对齐与长度"]
CheckAlign --> Valid{"访问有效?"}
Valid --> |否| Discard["丢弃写/返回0"]
Valid --> |是| ReadWrite{"读还是写?"}
ReadWrite --> |读| ReadPath["从寄存器文件读取"]
ReadPath --> DebugRead["可选调试输出"]
DebugRead --> Return["返回值"]
ReadWrite --> |写| Merge["合并半字写(如适用)"]
Merge --> Switch["根据偏移分派到具体寄存器"]
Switch --> Apply["应用写入逻辑(如队列CSR/DDTP)"]
Apply --> DebugWrite["可选调试输出"]
DebugWrite --> End(["结束"])
```

**图表来源**
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

**章节来源**
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

### 请求/响应接口与协议
- 请求格式
  - 主桥到IOMMU：device_id/pid_valid/process_id/no_write/exec_req/priv_req/is_cxl_dev + iommu_trans_req_t（at/iova/length/read_writeAMO）
- 响应格式
  - 状态：SUCCESS/UNSUPPORTED_REQUEST/COMPLETER_ABORT
  - 翻译响应：PPN/S/N/Priv/U/R/W/Exe/PBMT/is_msi/is_mrif/dest_mrif_addr/mrif_nid/is_bare_mode/pa
- 协议要点
  - ATS翻译请求、已翻译请求、未翻译请求三类地址类型
  - 执行/读/写/AMO语义由read_writeAMO与exec_req组合决定
  - Bare模式下仅允许未翻译请求，且pa=iova

```mermaid
sequenceDiagram
participant HB as "主桥/设备"
participant API as "IOMMU参考API"
participant FM as "功能模型"
participant MEM as "系统内存"
HB->>API : "hb_to_iommu_req_t"
API->>FM : "iommu_translate_iova(req, rsp)"
FM->>FM : "分类事务类型/提取属性"
FM->>MEM : "读取DC/PC/页表(隐式/显式)"
MEM-->>FM : "返回PTE/页表项"
FM->>FM : "两阶段/第二阶段/MSI翻译"
FM-->>API : "iommu_to_hb_rsp_t"
API-->>HB : "状态+翻译响应"
```

**图表来源**
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

**章节来源**
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

### 地址翻译与上下文定位
- 定位设备上下文
  - 根据device_id与PASID（pid_valid/process_id）在设备目录表中查找DC
  - 检查设备ID宽度与当前DDT模式匹配性
- 定位进程上下文
  - 依据DC中的pdtp与process_id在PDT中定位PC
  - 支持1/2/3级目录，隐式G-stage翻译根地址
- 两阶段地址翻译
  - VS-stage（S/VS）+ G-stage（Guest）两层页表
  - 支持原子更新A/D位（SADE/GADE），SXL控制虚拟地址范围
- 第二阶段翻译
  - 针对GPA进行G-stage翻译，返回G-PTE与页大小
- MSI地址翻译
  - 基于msi_addr_mask与msi_addr_pattern识别MSI写入
  - 支持Flat/Off模式，MRIF模式可直接路由至虚拟中断文件

```mermaid
flowchart TD
A["输入: IOVA/DID/PID/权限"] --> B["定位DC(设备上下文)"]
B --> C{"DDT模式/设备ID宽度合法?"}
C --> |否| F["报告故障/停止"]
C --> |是| D["定位PC(进程上下文)"]
D --> E{"两阶段翻译"}
E --> G["VS-stage -> G-stage"]
G --> H["返回PA/GPA/页大小/PTE"]
H --> I["构造响应/写入FQ/产生中断"]
```

**图表来源**
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)

**章节来源**
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)

### 故障处理与中断
- 故障类型
  - 访问故障、数据损坏、G-stage页/访问/数据损坏、MSI相关等
  - 故障队列记录包含cause/pid/pv/priv/ttyp/did/iotval/iotval2
- 中断生成
  - 命令队列/故障队列/HPM/页面请求队列中断源
  - 通过generate_interrupt/release_pending_interrupt管理
- 异常处理流程
  - 翻译过程中检测到故障即停止并上报，必要时写入FQ并产生中断

```mermaid
sequenceDiagram
participant FM as "功能模型"
participant FQ as "故障队列"
participant INT as "中断控制器"
FM->>FM : "检测故障(页表/访问/数据)"
FM->>FQ : "写入故障记录"
FM->>INT : "生成中断(若使能)"
INT-->>FM : "中断挂起状态更新"
```

**图表来源**
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

**章节来源**
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)

### 命令队列与ATS
- 命令类型
  - IOTINVAL（VMA/GVMA无效化）、IOFENCE（全局/地址范围/设备/功能/工作集无效化）、IODIR（目录无效化）、ATS（地址转换服务）
- ATS消息
  - 支持无效化与PRGR（Page Request Group Response）消息
- 阻塞与排队
  - ATS无效化请求可能被阻塞，需排队处理

```mermaid
classDiagram
class 命令队列命令 {
+opcode : 7b
+func3 : 3b
+av : 1b
+其他字段...
}
class IOTINVAL {
+GV : 1b
+AV : 1b
+NL : 1b
+PSCV/GSCID/PSCID/ADDR/S
}
class IOFENCE {
+PR/PW/AV/WSI/ADDR/DATA
}
class IODIR {
+PID/PV/DV/DID
}
class ATS {
+MSGCODE/TAG/DSV/DSEG/RID/PV/PID/PAYLOAD
}
命令队列命令 <|-- IOTINVAL
命令队列命令 <|-- IOFENCE
命令队列命令 <|-- IODIR
命令队列命令 <|-- ATS
```

**图表来源**
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)

**章节来源**
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)

## 依赖关系分析
- 头文件依赖
  - iommu_struct.hh前置包含寄存器、数据结构、请求/响应、故障、中断、命令队列、ATS、性能模型等头文件
  - iommu_task.hh依赖数据结构、翻译、请求/响应、性能参数
  - iommu_translate.hh对外提供翻译接口，内部依赖数据结构与工具
- 实现文件依赖
  - iommu_translate.cc依赖结构体、寄存器、故障、中断、命令队列、ATS、工具等
  - iommu_process_context.cc依赖结构体、寄存器、工具、翻译接口
  - iommu_reg.cc依赖寄存器定义与结构体

```mermaid
graph LR
A["iommu_struct.hh"] --> B["iommu_task.hh"]
A --> C["iommu_registers.hh"]
A --> D["iommu_data_structures.hh"]
A --> E["iommu_req_rsp.hh"]
A --> F["iommu_fault.hh"]
A --> G["iommu_interrupt.hh"]
A --> H["iommu_command_queue.hh"]
A --> I["iommu_ats.hh"]
A --> J["iommu_atc.hh"]
A --> K["iommu_hpm.hh"]
A --> L["iommu_ref_api.hh"]
B --> M["iommu_translate.cc"]
D --> M
E --> M
F --> M
G --> M
H --> M
I --> M
J --> M
K --> M
L --> M
C --> N["iommu_reg.cc"]
```

**图表来源**
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

**章节来源**
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [iommu_req_rsp.hh](file://iommu/include/iommu_req_rsp.hh)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

## 性能考量
- 事件计数与HPM
  - 通过HPM计数器与事件选择器统计各类访问与行走事件
  - 可通过iocntovf汇总溢出状态
- 缓存与命中率
  - DDT/PDT/TLB/PC缓存降低内存访问次数
  - 性能模型提供VPN提取、裸页大小、MSI地址判定等辅助
- 写入合并与对齐
  - 4B写入8B寄存器时进行半字合并，避免部分更新导致的不确定性
- 读写路径优化
  - 通过QoS ID（rcid/mcid）与端到端一致性，减少不必要的内存屏障

**章节来源**
- [iommu_perf_model.hh](file://iommu/include/iommu_perf_model.hh)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

## 故障排查指南
- 常见故障原因
  - 事务类型不支持（Off/Bare模式限制）
  - 设备ID宽度超出DDT模式支持范围
  - DC/PC未命中或配置错误（V位、保留位、PDT条目合法性）
  - 页表访问故障（访问/数据损坏/页故障）
  - ATS/PRG相关超时或不一致
- 定位步骤
  - 检查寄存器访问是否对齐与越界
  - 查看cqcsr/fqcsr/pqcsr状态位（cqmf/cmd_to/cmd_ill/fqof/pqof等）
  - 读取FQ记录（cause/pid/pv/priv/ttyp/did/iotval/iotval2）
  - 触发调试输出（DDTP读写调试打印）
- 建议
  - 在修改ddtp/cqcsr等寄存器前确认busy/cqon状态
  - 合理配置fctl.end与fctl.gxl以适配目标平台

**章节来源**
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)

## 结论
本API参考文档系统梳理了RISC-V IOMMU的核心数据结构、寄存器接口、请求/响应协议与翻译流程，并提供了接口规范、使用模式与性能优化建议。通过明确的任务状态机、严格的上下文与页表控制以及完善的故障与中断机制，IOMMU能够稳定地支撑两阶段地址翻译与MSI处理。

## 附录

### 接口规范与示例路径
- 翻译入口
  - 函数：iommu_translate_iova
  - 参数：iommu_t*、hb_to_iommu_req_t*、iommu_to_hb_rsp_t*
  - 示例路径：[翻译入口实现](file://iommu/iommu_fun_model/iommu_translate.cc)
- 上下文定位
  - locate_device_context / locate_process_context
  - 示例路径：[上下文定位实现](file://iommu/iommu_fun_model/iommu_process_context.cc)
- 寄存器访问
  - read_register / write_register
  - 示例路径：[寄存器访问实现](file://iommu/iommu_perf_model/iommu_reg.cc)
- 故障上报
  - report_fault
  - 示例路径：[故障上报声明](file://iommu/include/iommu_fault.hh)
- 中断管理
  - generate_interrupt / release_pending_interrupt
  - 示例路径：[中断接口声明](file://iommu/include/iommu_interrupt.hh)
- 命令处理
  - process_commands / do_iofence_c / do_iotinval_vma 等
  - 示例路径：[命令队列接口声明](file://iommu/include/iommu_command_queue.hh)

**章节来源**
- [iommu_ref_api.hh](file://iommu/include/iommu_ref_api.hh)
- [iommu_translate.cc](file://iommu/iommu_fun_model/iommu_translate.cc)
- [iommu_process_context.cc](file://iommu/iommu_fun_model/iommu_process_context.cc)
- [iommu_reg.cc](file://iommu/iommu_perf_model/iommu_reg.cc)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_interrupt.hh](file://iommu/include/iommu_interrupt.hh)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)

### 版本兼容性与迁移指南
- 版本字段
  - capabilities.version：记录实现遵循的规范版本（高4位为主版本，低4位为次版本）
- 兼容性建议
  - 当ddtp.iommu_mode处于非Off状态时，不建议动态切换特性（如fctl.be/gxl），否则行为未指定
  - 若需变更ddtp，应等待busy与cqon均为0后再进行
- 迁移提示
  - 从旧实现升级时，优先验证DDT模式与设备ID宽度匹配
  - ATS相关功能（EN_ATS/T2GPA）需与宿主/固件配合配置

**章节来源**
- [iommu_registers.hh](file://iommu/include/iommu_registers.hh)