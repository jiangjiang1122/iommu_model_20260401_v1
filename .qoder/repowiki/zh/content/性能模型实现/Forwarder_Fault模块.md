# Forwarder/Fault模块

<cite>
**本文档引用的文件**
- [iommu_perf_forwarder_fault_cq.cc](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc)
- [iommu_faults.cc](file://iommu/iommu_fun_model/iommu_faults.cc)
- [iommu_fault.hh](file://iommu/include/iommu_fault.hh)
- [iommu_command_queue.cc](file://iommu/iommu_perf_model/iommu_command_queue.cc)
- [iommu_command_queue.hh](file://iommu/include/iommu_command_queue.hh)
- [iommu_translate.hh](file://iommu/include/iommu_translate.hh)
- [iommu_top.hh](file://iommu/iommu_top.hh)
- [iommu_struct.hh](file://iommu/include/iommu_struct.hh)
- [iommu_perf_params.hh](file://iommu/iommu_perf_model/iommu_perf_params.hh)
- [iommu_perf_reorder.cc](file://iommu/iommu_perf_model/iommu_perf_reorder.cc)
- [iommu_perf_model.hh](file://iommu/iommu_perf_model/iommu_perf_model.hh)
- [iommu_top.cc](file://iommu/iommu_top.cc)
- [stats_collector.cpp](file://iommu/cache_src/common/stats_collector.cpp)
- [stats_collector.h](file://iommu/cache_src/common/stats_collector.h)
</cite>

## 更新摘要
**变更内容**
- 移除了Forwarder线程中的串行延迟（FORWARDER_DELAY），提升了转发性能
- 新增了全面的峰值统计功能，包括outstanding峰值监控
- 增强了性能监控能力，支持更详细的性能分析
- 更新了性能考量部分以反映最新的优化改进

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构概览](#架构概览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能考量](#性能考量)
8. [故障处理指南](#故障处理指南)
9. [结论](#结论)

## 简介
本文档深入解析IOMMU性能模型中的Forwarder/Fault模块实现机制。该模块负责数据包转发和故障处理两大核心功能，通过三个独立线程实现高性能的数据转发（PT Cache和MSIPT Cache路径）以及故障记录与命令队列处理。文档详细说明了数据包转发流程、命令队列处理、故障检测与报告机制，并提供故障类型处理策略、错误码定义和恢复机制的完整说明。

**更新** 本版本反映了最新的性能优化改进，包括移除串行延迟和新增峰值统计功能。

## 项目结构
Forwarder/Fault模块位于IOMMU性能模型子系统中，采用SystemC多线程架构设计，包含以下关键组件：

```mermaid
graph TB
subgraph "Forwarder/Fault模块"
PTF["PT Forwarder<br/>PT Cache转发器"]
MSF["MSIPT Forwarder<br/>MSIPT Cache转发器"]
FCQ["Fault/CQ处理线程<br/>故障记录+命令队列处理"]
end
subgraph "性能优化参数"
FP["移除串行延迟<br/>FORWARDER_DELAY=0ns"]
RD["重排序输出延迟<br/>1ns"]
OOL["全局outstanding上限<br/>256"]
end
subgraph "峰值统计监控"
PS["峰值统计追踪器<br/>peak_*_outstanding"]
PP["性能监控<br/>详细统计报告"]
end
subgraph "数据结构"
TF["任务结构体<br/>iommu_task_t"]
FR["故障记录<br/>fault_rec_t"]
RC["重排序缓冲区<br/>reorder_buf"]
end
PTW["PT Cache"] --> PTW2["PTW"]
MSW["MSIPT Cache"] --> MSW2["MSIPTW"]
PTW2 --> PTW3["Forwarder"]
MSW2 --> MSW3["Forwarder"]
PTW3 --> FCQ
MSW3 --> FCQ
FCQ --> RC
RC --> TF
```

**图表来源**
- [iommu_perf_forwarder_fault_cq.cc:13-179](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L13-L179)
- [iommu_top.hh:251-257](file://iommu/iommu_top.hh#L251-L257)
- [iommu_top.hh:153-161](file://iommu/iommu_top.hh#L153-L161)

**章节来源**
- [iommu_top.hh:251-257](file://iommu/iommu_top.hh#L251-L257)
- [iommu_perf_params.hh:131](file://iommu/iommu_perf_model/iommu_perf_params.hh#L131)

## 核心组件
Forwarder/Fault模块包含三个核心线程，每个线程都有特定的职责和处理逻辑：

### PT Forwarder线程
负责PT Cache路径的翻译结果转发，将地址转换后的请求通过PCIe NoC路由到目标设备。

### MSIPT Forwarder线程  
负责MSIPT Cache路径的翻译结果转发，根据MSI/MRIF标志位决定数据传输路径。

### Fault/CQ处理线程
负责故障记录、ATS请求分类处理以及命令队列的执行控制。

**更新** 所有Forwarder线程现在移除了串行延迟，提升了转发效率。

**章节来源**
- [iommu_perf_forwarder_fault_cq.cc:13-179](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L13-L179)

## 架构概览
Forwarder/Fault模块采用流水线架构设计，通过FIFO缓冲区连接各个处理阶段：

```mermaid
sequenceDiagram
participant Collector as "收集器"
participant PTW as "PT Cache"
participant MSIPTW as "MSIPT Cache"
participant PTForwarder as "PT转发器"
participant MSIPTForwarder as "MSIPT转发器"
participant FaultCQ as "故障/CQ处理"
participant Reorder as "重排序缓冲区"
participant Initiator as "发起者"
Collector->>PTW : 发送PT Cache查询
Collector->>MSIPTW : 发送MSIPT Cache查询
PTW-->>Collector : 返回PT Cache响应
MSIPTW-->>Collector : 返回MSIPT Cache响应
Collector->>PTForwarder : 发送PT Cache转发请求
Collector->>MSIPTForwarder : 发送MSIPT Cache转发请求
Collector->>FaultCQ : 发送故障处理请求
PTForwarder->>Reorder : 标记任务就绪无延迟
MSIPTForwarder->>Reorder : 标记任务就绪无延迟
FaultCQ->>Reorder : 标记任务就绪无延迟
Reorder->>Initiator : 按规则输出响应
```

**图表来源**
- [iommu_perf_forwarder_fault_cq.cc:13-179](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L13-L179)
- [iommu_top.hh:185-206](file://iommu/iommu_top.hh#L185-L206)

## 详细组件分析

### PT Forwarder组件分析

PT Forwarder线程实现了PT Cache路径的高效数据转发，现已移除串行延迟：

```mermaid
flowchart TD
Start([开始处理]) --> CheckFIFO["检查pt_cache_to_fwd_fifo"]
CheckFIFO --> HasData{"FIFO是否有数据?"}
HasData --> |否| WaitEvent["等待数据写入事件"]
HasData --> |是| ReadTask["读取任务"]
ReadTask --> SetState["设置任务状态为TASK_FORWARD"]
SetState --> CheckTrans{"是否有TLM负载?"}
CheckTrans --> |否| MarkReady["标记重排序就绪"]
CheckTrans --> |是| SetResponse["设置响应状态为TLM_OK_RESPONSE"]
SetResponse --> SetAddress["设置转换后的物理地址"]
SetAddress --> SendResponse["标记重排序就绪"]
SendResponse --> ReleaseTask["等待重排序输出线程释放"]
ReleaseTask --> End([结束])
WaitEvent --> ReadTask
```

**更新** 移除了原有的2ns串行延迟，直接标记任务就绪，提升了转发效率。

**图表来源**
- [iommu_perf_forwarder_fault_cq.cc:13-53](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L13-L53)

**章节来源**
- [iommu_perf_forwarder_fault_cq.cc:13-53](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L13-L53)

### MSIPT Forwarder组件分析

MSIPT Forwarder线程处理MSI和MRIF类型的地址转换，同样移除了串行延迟：

```mermaid
flowchart TD
Start([开始处理]) --> CheckFIFO["检查msipt_cache_to_fwd_fifo"]
CheckFIFO --> HasData{"FIFO是否有数据?"}
HasData --> |否| WaitEvent["等待数据写入事件"]
HasData --> |是| ReadTask["读取任务"]
ReadTask --> SetState["设置任务状态为TASK_FORWARD"]
SetState --> CheckTrans{"是否有TLM负载?"}
CheckTrans --> |否| MarkReady["标记重排序就绪"]
CheckTrans --> |是| SetResponse["设置响应状态为TLM_OK_RESPONSE"]
SetResponse --> SetAddress["设置转换后的物理地址"]
SetAddress --> CheckFlags{"检查MSI/MRIF标志"}
CheckFlags --> IsMSI{"is_msi=1且is_mrif=0?"}
IsMSI --> |是| SendToIMSI["通过axi_stream_socket发送到IMSI"]
IsMSI --> |否| SendToDDR["通过axi_master_1发送到DDR/CMN"]
SendToIMSI --> MarkReady
SendToDDR --> MarkReady
MarkReady --> ReleaseTask["等待重排序输出线程释放"]
ReleaseTask --> End([结束])
WaitEvent --> ReadTask
```

**更新** 移除了原有的2ns串行延迟，直接进行路由决策和标记就绪。

**图表来源**
- [iommu_perf_forwarder_fault_cq.cc:62-114](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L62-L114)

**章节来源**
- [iommu_perf_forwarder_fault_cq.cc:62-114](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L62-L114)

### Fault/CQ处理组件分析

Fault/CQ处理线程实现了复杂的故障分类和命令队列处理逻辑，移除了串行延迟：

```mermaid
flowchart TD
Start([开始处理]) --> ReadFIFO["读取collector_to_fault_fifo"]
ReadFIFO --> LogFault["调用report_fault记录故障"]
LogFault --> CheckTrans{"是否有TLM负载?"}
CheckTrans --> |否| DeleteTask["删除任务并返回"]
CheckTrans --> |是| CheckATS{"是否为PCIe ATS请求?"}
CheckATS --> |否| SetUR["设置响应状态为GENERIC_ERROR_RESPONSE"]
CheckATS --> |是| ClassifyCause["按cause代码分类"]
ClassifyCause --> SuccessWithRW["R=W=0的成功响应"]
ClassifyCause --> CompleterAbort["Completer Abort响应"]
ClassifyCause --> UnsupportedRequest["Unsupported Request响应"]
SuccessWithRW --> SetOK["设置响应状态为OK_RESPONSE"]
CompleterAbort --> SetUR
UnsupportedRequest --> SetUR
SetOK --> MarkReady["标记重排序就绪"]
SetUR --> MarkReady
MarkReady --> End([结束])
DeleteTask --> End
```

**更新** 移除了原有的2ns串行延迟，直接进行故障记录和响应分类。

**图表来源**
- [iommu_perf_forwarder_fault_cq.cc:121-179](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L121-L179)

**章节来源**
- [iommu_perf_forwarder_fault_cq.cc:121-179](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L121-L179)

### 故障记录机制

故障记录机制遵循SPEC Section 12.17-12.18规范，实现了完整的故障队列管理：

```mermaid
classDiagram
class fault_rec_t {
+uint64_t CAUSE : 12
+uint64_t PID : 20
+uint64_t PV : 1
+uint64_t PRIV : 1
+uint64_t TTYP : 6
+uint64_t DID : 24
+uint32_t custom
+uint32_t reserved
+uint64_t iotval
+uint64_t iotval2
}
class iommu_regs_t {
+fqh : index
+fqt : index
+fqb : ppn
+fqcsr : bits
}
class report_fault {
+check_fq_enable()
+check_fqmf_bit()
+check_fqof_bit()
+check_dtf_bit()
+write_fault_record()
+generate_interrupt()
}
fault_rec_t --> iommu_regs_t : "写入到内存队列"
report_fault --> fault_rec_t : "创建故障记录"
report_fault --> iommu_regs_t : "更新寄存器状态"
```

**图表来源**
- [iommu_faults.cc:8-160](file://iommu/iommu_fun_model/iommu_faults.cc#L8-L160)
- [iommu_fault.hh:63-77](file://iommu/include/iommu_fault.hh#L63-L77)

**章节来源**
- [iommu_faults.cc:8-160](file://iommu/iommu_fun_model/iommu_faults.cc#L8-L160)
- [iommu_fault.hh:63-77](file://iommu/include/iommu_fault.hh#L63-L77)

### ATS请求分类处理

ATS请求根据cause代码进行精确分类，实现SPEC要求的响应行为：

| 分类 | Cause代码范围 | 响应类型 | 行为描述 |
|------|---------------|----------|----------|
| 成功响应(R=W=0) | 12, 13, 15, 20, 21, 23, 266, 262 | OK_RESPONSE | 返回成功但R=W=0 |
| Completer Abort | 1, 5, 7, 261, 263, 265, 267, 268, 269, 270, 271, 272, 274 | GENERIC_ERROR_RESPONSE | 返回CA响应 |
| Unsupported Request | 256, 257, 258, 259, 260 | GENERIC_ERROR_RESPONSE | 返回UR响应 |

**章节来源**
- [iommu_perf_forwarder_fault_cq.cc:152-170](file://iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc#L152-L170)
- [iommu_translate.cc:657-732](file://iommu/iommu_fun_model/iommu_translate.cc#L657-L732)

## 依赖关系分析

Forwarder/Fault模块与系统其他组件存在紧密的依赖关系：

```mermaid
graph TB
subgraph "Forwarder/Fault模块"
PTForwarder["pt_forwarder_thread"]
MSIPTForwarder["msipt_forwarder_thread"]
FaultCQ["fault_cq_proc_thread"]
end
subgraph "系统组件"
Reorder["重排序缓冲区"]
FIFOs["FIFO缓冲区"]
Sockets["AXI Socket接口"]
Interrupt["中断控制器"]
end
subgraph "数据结构"
Task["iommu_task_t"]
FaultRec["fault_rec_t"]
ReorderEntry["reorder_entry_t"]
end
PTForwarder --> Reorder
MSIPTForwarder --> Reorder
FaultCQ --> Reorder
Reorder --> FIFOs
Reorder --> Sockets
Reorder --> Interrupt
PTForwarder --> Task
MSIPTForwarder --> Task
FaultCQ --> Task
Reorder --> ReorderEntry
Task --> FaultRec
```

**图表来源**
- [iommu_top.hh:69-114](file://iommu/iommu_top.hh#L69-L114)
- [iommu_top.hh:185-206](file://iommu/iommu_top.hh#L185-L206)

**章节来源**
- [iommu_top.hh:69-114](file://iommu/iommu_top.hh#L69-L114)
- [iommu_top.hh:185-206](file://iommu/iommu_top.hh#L185-L206)

## 性能考量

Forwarder/Fault模块在设计时充分考虑了性能优化，最新版本移除了串行延迟并增强了监控能力：

### 转发延迟分析
- **Forwarder延迟**: 已移除（设为0ns），确保零延迟数据转发
- **重排序输出延迟**: 1ns，维持输出节拍一致性
- **全局outstanding上限**: 256，防止内存溢出

### 峰值统计监控
**新增** 全面的峰值统计功能，实时监控系统各组件的峰值使用情况：

```mermaid
graph TB
subgraph "峰值统计追踪器"
PG["IOMMU Global<br/>peak_iommu_global_outstanding"]
PT["PTW<br/>peak_ptw_outstanding"]
XD["xDTW DC walk<br/>peak_xdtw_dc_outstanding"]
XP["xDTW PC walk<br/>peak_xdtw_pc_outstanding"]
CD["Collector DC walk<br/>peak_collector_dc_walk_outstanding"]
CP["Collector PC walk<br/>peak_collector_pc_walk_outstanding"]
MD["DDR (master_1)<br/>peak_axi_master_1_outstanding"]
MO["Output (master_0)<br/>peak_axi_master_0_outstanding"]
end
subgraph "监控输出"
PO["Outstanding Peak Statistics<br/>峰值统计报告"]
end
PG --> PO
PT --> PO
XD --> PO
XP --> PO
CD --> PO
CP --> PO
MD --> PO
MO --> PO
```

**图表来源**
- [iommu_top.hh:153-161](file://iommu/iommu_top.hh#L153-L161)
- [iommu_top.cc:622-640](file://iommu/iommu_top.cc#L622-L640)

### 队列管理策略
- **写保序读乱序**: 写请求严格按task_id保序，读请求可乱序输出
- **FIFO深度配置**: 各个FIFO都有SPEC定义的深度限制
- **并发流控**: 通过outstanding计数器控制AXI端口并发度

### 性能监控方法
**增强** 新增详细的性能监控能力：

#### 峰值统计报告
- **IOMMU全局峰值**: 实时监控全局outstanding使用峰值
- **各组件峰值**: PTW、xDTW、Collector等组件的outstanding峰值
- **端口峰值**: DDR和输出端口的outstanding峰值

#### 传统性能指标
- **IOMMU完成计数**: `iommu_total_completed`统计翻译完成数量
- **缓存命中率**: 全局统计变量跟踪各缓存命中情况
- **字节计数器**: 监控各端口数据流量

**章节来源**
- [iommu_perf_params.hh:131](file://iommu/iommu_perf_model/iommu_perf_params.hh#L131)
- [iommu_perf_reorder.cc:89-154](file://iommu/iommu_perf_model/iommu_perf_reorder.cc#L89-L154)
- [iommu_top.hh:144-152](file://iommu/iommu_top.hh#L144-L152)
- [iommu_top.cc:622-640](file://iommu/iommu_top.cc#L622-L640)

## 故障处理指南

### 故障类型处理策略

#### 地址转换故障
- **访问故障**: 设置ACCESS_FAULT标志，触发故障记录
- **数据损坏**: 标记DATA_CORRUPTION，生成相应中断
- **页面故障**: 根据访问类型设置不同cause码

#### ATS事务故障
- **配置错误**: 返回Completer Abort响应
- **永久错误**: 返回Unsupported Request响应
- **成功但R=W=0**: 返回成功但不设置读写权限

### 错误码定义

| Cause码 | 故障类型 | 描述 | 处理方式 |
|---------|----------|------|----------|
| 1, 5, 7 | 访问故障 | 指令/读/写访问故障 | CA响应 |
| 12, 13, 15 | 页面故障 | 指令/读/写页面故障 | 成功但R=W=0 |
| 20, 21, 23 | 客户端页面故障 | 客户端指令/读/写页面故障 | 成功但R=W=0 |
| 256-260 | 交易类型错误 | 交易类型不允许 | UR响应 |
| 261-274 | MSI/PT故障 | MSI PTE/PDT/PT数据损坏 | CA响应 |

### 恢复机制

#### 故障队列溢出处理
- **fqof标志**: 队列满时设置，停止故障记录
- **fqmf标志**: 内存访问故障时设置，停止故障记录
- **软件清理**: 通过写入清除位恢复故障记录功能

#### 命令队列故障处理
- **cqmf标志**: 命令队列内存故障时设置
- **cmd_ill标志**: 非法命令时设置
- **cmd_to标志**: 命令超时处理

**章节来源**
- [iommu_faults.cc:23-44](file://iommu/iommu_fun_model/iommu_faults.cc#L23-L44)
- [iommu_faults.cc:129-133](file://iommu/iommu_fun_model/iommu_faults.cc#L129-L133)
- [iommu_command_queue.cc:35-43](file://iommu/iommu_perf_model/iommu_command_queue.cc#L35-L43)

## 结论

Forwarder/Fault模块通过精心设计的多线程架构和严格的SPEC实现，提供了高效的IOMMU数据转发和故障处理能力。最新版本的性能优化显著提升了系统性能：

### 主要性能改进
1. **零延迟转发**: 移除所有Forwarder线程的串行延迟，提升转发效率
2. **全面峰值监控**: 新增详细的峰值统计功能，实时监控系统使用情况
3. **增强性能分析**: 提供更丰富的性能监控指标和统计报告

### 模块优势
1. **高并发处理**: 三个独立线程并行处理不同类型的任务
2. **精确的故障分类**: 基于SPEC的完整故障处理策略
3. **灵活的队列管理**: 支持写保序读乱序的输出机制
4. **完善的性能监控**: 提供详细的统计和监控指标

**更新** 该模块为IOMMU性能模型提供了坚实的基础设施，确保了系统的可靠性和高性能运行，同时通过移除串行延迟和新增峰值统计功能进一步提升了性能表现。