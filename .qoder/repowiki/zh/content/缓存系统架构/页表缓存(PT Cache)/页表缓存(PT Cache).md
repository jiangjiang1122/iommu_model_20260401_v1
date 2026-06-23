# 页表缓存(PT Cache)

<cite>
**本文档引用的文件**
- [pt_cache.h](file://iommu/cache_src/cache/pt_cache.h)
- [pt_cache.cpp](file://iommu/cache_src/cache/pt_cache.cpp)
- [cache_base.h](file://iommu/cache_src/cache/cache_base.h)
- [cache_base.cpp](file://iommu/cache_src/cache/cache_base.cpp)
- [cache_line.h](file://iommu/cache_src/cache/cache_line.h)
- [types.h](file://iommu/cache_src/common/types.h)
- [stats_collector.h](file://iommu/cache_src/common/stats_collector.h)
- [stats_collector.cpp](file://iommu/cache_src/common/stats_collector.cpp)
- [cache_subsystem.h](file://iommu/cache_src/subsystem/cache_subsystem.h)
- [cache_subsystem.cpp](file://iommu/cache_src/subsystem/cache_subsystem.cpp)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [json_config.cpp](file://iommu/cache_src/common/json_config.cpp)
- [cache_subsystem.cpp](file://iommu/cache_src/subsystem/cache_subsystem.cpp)
- [iommu_perf_pt_cache_response.cc](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc)
- [iommu_task_cache_convert.cc](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc)
- [iommu_task_cache_convert.hh](file://iommu/iommu_perf_model/iommu_task_cache_convert.hh)
- [iommu_top.hh](file://iommu/iommu_top.hh)
- [iommu_top.cc](file://iommu/iommu_top.cc)
- [iommu_perf_params.hh](file://iommu/iommu_perf_model/iommu_perf_params.hh)
- [iommu_task.hh](file://iommu/include/iommu_task.hh)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)
- [PT_CACHE_AD_BIT_MODIFICATION.md](file://PT_CACHE_AD_BIT_MODIFICATION.md)
- [dedup_buffer.h](file://iommu/cache_src/common/dedup_buffer.h)
- [PT_CACHE_DEDUP_PREFETCH_INTEGRATED_V2_20260608.md](file://PT_CACHE_DEDUP_PREFETCH_INTEGRATED_V2_20260608.md)
- [PTW_RESPONSE_ANALYSIS_20260609.md](file://PTW_RESPONSE_ANALYSIS_20260609.md)
- [PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md](file://PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md)
- [VA_DEDUP_VS_NO_DEDUP_COMPARISON.md](file://VA_DEDUP_VS_NO_DEDUP_COMPARISON.md)
- [TEST_50_PACKETS_D3_ANALYSIS_20260609.md](file://TEST_50_PACKETS_D3_ANALYSIS_20260609.md)
- [PT_DEDUP_PREFETCH_FINAL_SCHEME.md](file://PT_DEDUP_PREFETCH_FINAL_SCHEME.md)
- [PERF_MODEL_IMPLEMENTATION.md](file://PERF_MODEL_IMPLEMENTATION.md)
- [PTW_PREFETCH_BURST_OPTIMIZATION_20260608.md](file://PTW_PREFETCH_BURST_OPTIMIZATION_20260608.md)
- [PT_CACHE_PREFETCH_IMPLEMENTATION_ANALYSIS_20260609.md](file://PT_CACHE_PREFETCH_IMPLEMENTATION_ANALYSIS_20260609.md)
</cite>

## 更新摘要
**所做更改**
- 新增单线程调度器架构章节，详细描述统一轮询调度机制和任务级串行处理
- 增强统计收集系统，新增相位时间戳记录和任务等待时间统计
- 修复PT Cache替换保护机制bug，完善占位CL保护策略
- 增强性能报告系统，新增PT Cache性能摘要和任务放大系数分析
- 更新PT Cache优化改进，包含预取功能实现和Buffer链表管理

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [单线程调度器架构](#单线程调度器架构)
7. [增强统计收集系统](#增强统计收集系统)
8. [PT Cache替换保护机制](#pt-cache替换保护机制)
9. [性能报告系统增强](#性能报告系统增强)
10. [PT Cache优化改进](#pt-cache优化改进)
11. [A/D位支持功能](#ad位支持功能)
12. [VA去重功能](#va去重功能)
13. [占位CL与去重缓冲区](#占位cl与去重缓冲区)
14. [依赖关系分析](#依赖关系分析)
15. [性能考量](#性能考量)
16. [故障排查指南](#故障排查指南)
17. [结论](#结论)
18. [附录](#附录)

## 简介
本文件针对 RISC-V IOMMU 的页表缓存(PT Cache)进行系统化技术文档化，重点阐述其作为 IOTLB(IOMMU Page Table Lookaside Buffer)的核心作用：缓存页表项并加速地址转换。文档覆盖以下方面：
- PT Cache 的数据结构设计与缓存行组织方式
- 查找、插入与更新的处理流程与命中/未命中机制
- 失效管理策略（VMA/GVMA/按上下文批量失效）
- **新增单线程调度器架构**：通过统一轮询调度机制实现任务级串行处理，避免互锁死锁问题
- **增强统计收集系统**：新增相位时间戳记录和任务等待时间统计，提供更精确的性能分析
- **PT Cache替换保护机制修复**：完善占位CL保护策略，解决Cache满时的替换问题
- **性能报告系统增强**：新增PT Cache性能摘要和任务放大系数分析
- **PT Cache优化改进**：实现预取功能、Buffer链表管理和批量更新机制
- **新增A/D位支持功能**：通过A/D位检查机制实现内存访问跟踪，完善PT Cache的访问监控能力
- **新增VA去重功能**：通过deduplication table实现同页VA访问的去重优化
- **新增占位CL与去重缓冲区功能**：通过占位缓存行和去重缓冲区实现预取优化和任务挂起管理
- 性能特征、命中率优化与内存占用评估
- 配置参数、替换策略选择与调优建议
- 通过代码路径引用展示关键操作的实现位置

## 项目结构
PT Cache 位于 iommu/cache_src/cache 子目录，采用模板化基类 CacheBase 提供统一的缓存接口与性能建模，PTCache 作为特化实现，结合公共类型定义与统计收集器完成端到端的功能。**新增的单线程调度器架构通过统一轮询调度机制实现任务级串行处理，避免传统多线程架构中的互锁死锁问题。**

```mermaid
graph TB
subgraph "缓存实现"
PT["PTCache<br/>pt_cache.cpp/.h"]
CB["CacheBase<Tag,Data><br/>cache_base.h/.cpp"]
CL["CacheLine<br/>cache_line.h"]
end
subgraph "单线程调度器"
SC["CacheSubsystem<br/>cache_subsystem.cpp/.h"]
PS["PT调度线程<br/>pt_scheduler_thread"]
WS["Walker调度线程<br/>walker_scheduler_thread"]
end
subgraph "增强统计系统"
ST["StatsCollector<br/>stats_collector.h/.cpp"]
TS["时间戳记录<br/>record_pt_phase_timestamp"]
TW["任务等待统计<br/>accumulate_phase_task_wait"]
end
subgraph "公共类型与配置"
TY["类型与数据结构<br/>types.h"]
JC["JSON配置解析<br/>json_config.cpp"]
SS["缓存子系统集成<br/>cache_subsystem.cpp"]
end
subgraph "性能与统计"
PM["性能模型响应<br/>iommu_perf_pt_cache_response.cc"]
end
subgraph "IOMMU规范类型"
DS["数据结构定义<br/>iommu_data_structures.hh"]
end
subgraph "A/D位支持"
AD["A/D位处理<br/>task_to_pt_update"]
AD2["A/D位检查<br/>pt_cache_response"]
end
subgraph "VA去重功能"
VD["VA去重表<br/>iommu_top.hh/.cc"]
VP["去重参数<br/>iommu_perf_params.hh"]
end
subgraph "占位CL与去重缓冲区"
PH["占位CL接口<br/>insert_placeholder/batch_update_placeholders"]
DB["去重缓冲区<br/>dedup_buffer.h"]
PF["预取管理<br/>flush_dedup_buffer_chain"]
end
PT --> CB
PT --> CL
PT --> TY
CB --> ST
SC --> PS
SC --> WS
PS --> ST
WS --> ST
SS --> PT
JC --> SS
PM --> PT
TY --> DS
PM --> VD
VP --> VD
AD --> PM
AD2 --> PM
PH --> PT
DB --> PF
PF --> PT
```

**图表来源**
- [pt_cache.h:1-72](file://iommu/cache_src/cache/pt_cache.h#L1-L72)
- [pt_cache.cpp:1-298](file://iommu/cache_src/cache/pt_cache.cpp#L1-L298)
- [cache_base.h:1-676](file://iommu/cache_src/cache/cache_base.h#L1-L676)
- [cache_line.h:1-103](file://iommu/cache_src/cache/cache_line.h#L1-L103)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [stats_collector.h:1-130](file://iommu/cache_src/common/stats_collector.h#L1-L130)
- [stats_collector.cpp:147-195](file://iommu/cache_src/common/stats_collector.cpp#L147-L195)
- [types.h:1-629](file://iommu/cache_src/common/types.h#L1-L629)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [iommu_perf_pt_cache_response.cc:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)
- [iommu_task_cache_convert.cc:240-300](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L240-L300)
- [iommu_top.hh:123-142](file://iommu/iommu_top.hh#L123-L142)
- [iommu_perf_params.hh:113-114](file://iommu/iommu_perf_model/iommu_perf_params.hh#L113-L114)
- [dedup_buffer.h:1-122](file://iommu/cache_src/common/dedup_buffer.h#L1-L122)

**章节来源**
- [pt_cache.h:1-72](file://iommu/cache_src/cache/pt_cache.h#L1-L72)
- [pt_cache.cpp:1-298](file://iommu/cache_src/cache/pt_cache.cpp#L1-L298)
- [cache_base.h:1-676](file://iommu/cache_src/cache/cache_base.h#L1-L676)
- [cache_line.h:1-103](file://iommu/cache_src/cache/cache_line.h#L1-L103)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [stats_collector.h:1-130](file://iommu/cache_src/common/stats_collector.h#L1-L130)
- [stats_collector.cpp:147-195](file://iommu/cache_src/common/stats_collector.cpp#L147-L195)
- [types.h:1-629](file://iommu/cache_src/common/types.h#L1-L629)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [iommu_perf_pt_cache_response.cc:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)
- [iommu_task_cache_convert.cc:240-300](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L240-L300)
- [iommu_top.hh:123-142](file://iommu/iommu_top.hh#L123-L142)
- [iommu_perf_params.hh:113-114](file://iommu/iommu_perf_model/iommu_perf_params.hh#L113-L114)
- [dedup_buffer.h:1-122](file://iommu/cache_src/common/dedup_buffer.h#L1-L122)

## 核心组件
- PTCache：PT Cache 的具体实现，继承自 CacheBase，提供页表项查找、填充与多种失效策略，**新增占位CL插入和批量更新接口**。
- CacheBase：通用缓存模板基类，封装哈希定位、set 内 way 比较、替换算法、仲裁与延迟建模等。
- CacheLine：缓存行模板，包含 valid 标志、tag、data、预取标记与访问计数。
- PTTag/PTData：PT Cache 的标签与数据结构，承载 gscid、pscid、IOVA、翻译阶段、SV48/X4 模式等关键信息。
- CacheConfig：缓存配置，包含 set 数、way 数、替换策略、仲裁与流水线延迟等。
- **新增StatsCollector**：增强统计收集器，记录访问、命中、缺失、替换、失效、队列延迟等指标，**新增相位时间戳记录和任务等待时间统计**。
- **新增CacheSubsystem**：单线程调度器架构，通过统一轮询调度机制实现任务级串行处理，**pt_scheduler_thread合并pt_worker_thread和pt_update_worker_thread**。
- **A/D位支持组件**：A/D位检查机制、动态A/D位传递、日志增强功能。
- **VA去重组件**：deduplication table、去重键值结构、同步机制和统计计数器。
- **占位CL组件**：insert_placeholder接口、batch_update_placeholders接口、占位CL数据结构。
- **去重缓冲区组件**：DedupBuffer类、Buffer链表管理、任务挂起与恢复机制。

**章节来源**
- [pt_cache.h:8-72](file://iommu/cache_src/cache/pt_cache.h#L8-L72)
- [pt_cache.cpp:5-39](file://iommu/cache_src/cache/pt_cache.cpp#L5-L39)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:70-91](file://iommu/cache_src/cache/cache_line.h#L70-L91)
- [types.h:33-45](file://iommu/cache_src/common/types.h#L33-L45)
- [types.h:573-589](file://iommu/cache_src/common/types.h#L573-L589)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [iommu_task_cache_convert.cc:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-291)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)
- [iommu_top.hh:123-142](file://iommu/iommu_top.hh#L123-L142)
- [dedup_buffer.h:13-122](file://iommu/cache_src/common/dedup_buffer.h#L13-L122)

## 架构总览
PT Cache 在 IOMMU 地址转换路径中扮演 IOTLB 角色，通过缓存页表项减少对主存页表的访问次数，提升整体吞吐。**新增的单线程调度器架构通过统一轮询调度机制实现任务级串行处理，避免传统多线程架构中的互锁死锁问题。**其核心流程：
- 查找：根据 gscid/pscid/iova/阶段/模式构造 PTTag，经哈希定位 set，遍历 way 完成 tag 比较，命中则返回 PTData 并更新替换策略。
- **A/D位检查**：在PT Cache命中时，检查A/D位状态，如果需要更新且SADE=1，则触发PTW进行硬件更新。
- **VA去重**：在PT Cache未命中时，检查VA去重表，如果同页VA访问已存在，则挂起当前任务等待去重完成。
- **占位CL处理**：在PT Cache未命中时，检查去重缓冲区，如果存在占位CL则直接返回占位CL，避免重复PTW。
- 填充：将 Walker 或页表遍历结果写入缓存，同时记录翻译阶段、输入/输出页大小、SV48/X4 等元信息。
- 失效：支持 VMA(G-stage)、GVMA(S-stage)、按 gscid/pscid 批量失效以及全局失效，按模式精确或扫描执行。

```mermaid
sequenceDiagram
participant Req as "请求方"
participant PT as "PTCache"
participant SC as "CacheSubsystem"
participant Dedup as "VA去重表"
participant PH as "占位CL/缓冲区"
participant Base as "CacheBase"
participant Set as "Set(多way)"
participant RP as "替换策略"
Req->>SC : "pt_request_fifo.write(request)"
SC->>SC : "pt_scheduler_thread轮询处理"
SC->>PT : "execute_pt_request(req)"
PT->>PT : "构造PTTag并页对齐"
PT->>Base : "lookup(tag, out_data, latency)"
Base->>Base : "hash(tag)->set"
Base->>Set : "find_way(set, tag)"
alt "命中"
Set-->>Base : "way索引"
Base->>RP : "access(set, way)"
Base-->>PT : "true, out_data"
PT->>PT : "检查A/D位状态"
alt "A/D需要更新且SADE=1"
PT-->>SC : "触发PTW进行A/D更新"
else "A/D已设置或SADE=0"
PT-->>SC : "直接转发到forwarder"
end
else "未命中"
Set-->>Base : "未找到"
Base-->>PT : "false"
PT->>Dedup : "检查VA去重表"
alt "去重命中"
Dedup-->>PT : "挂起任务，等待去重完成"
PT-->>SC : "未命中(等待去重)"
else "去重未命中"
PT->>PH : "检查占位CL/缓冲区"
alt "占位CL存在"
PH-->>PT : "返回占位CL"
PT-->>SC : "占位CL命中"
else "无占位CL"
PH-->>PT : "无占位CL"
PT-->>SC : "触发页表遍历"
end
end
end
SC-->>Req : "响应返回"
```

**图表来源**
- [pt_cache.cpp:11-23](file://iommu/cache_src/cache/pt_cache.cpp#L11-L23)
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)
- [iommu_task_cache_convert.cc:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-291)

**章节来源**
- [pt_cache.cpp:11-23](file://iommu/cache_src/cache/pt_cache.cpp#L11-L23)
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)
- [iommu_task_cache_convert.cc:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-291)

## 详细组件分析

### PTCache 类与接口
- 查找接口：支持带 SV48/X4 参数的查找重载，内部统一构造 PTTag 并调用基类 lookup。
- 填充接口：将 Walker 结果或页表遍历结果写入缓存，自动设置翻译阶段、IOVA 是否为 VA、SV48/X4 等元信息。
- 失效接口：
  - VMA 失效：影响第一阶段翻译，支持精确/扫描/全局模式。
  - GVMA 失效：影响第二阶段翻译。
  - 按上下文失效：按 gscid/pscid 批量失效。
  - 全局失效：清空所有有效行。
- **新增占位CL接口**：
  - insert_placeholder：插入占位缓存行，支持主任务占位（is_req=1）和预取占位（is_req=0）。
  - batch_update_placeholders：批量更新占位CL为常规CL，支持预取结果的快速填充。
- 哈希函数：基于 gscid/iova/psecid 组合进行混合哈希，掩码取 set 索引。
- 页对齐：按 4K 对齐 IOVA，确保同一页内的访问共享缓存行。

```mermaid
classDiagram
class PTCache {
+lookup_pt(gscid, pscid, iova, stage, sv48, gstage_x4, out_data, latency) bool
+fill_pt(gscid, pscid, iova, stage, data, from_prefetch) void
+invalidate_vma(gscid, pscid, iova, has_gscid, has_pscid, has_iova, mode, latency) uint32_t
+invalidate_gvma(gscid, gpa, has_gscid, has_gpa, mode, latency) uint32_t
+invalidate_by_gscid(gscid, latency) uint32_t
+invalidate_by_gscid_pscid(gscid, pscid, latency) uint32_t
+invalidate_global(latency) uint32_t
+insert_placeholder(gscid, pscid, iova, stage, sv48, gstage_x4, head_index, tail_index, is_req, latency) bool
+batch_update_placeholders(gscid, pscid, updates, stage, sv48, gstage_x4) void
-hash_function(tag) uint32_t
-align_iova(iova, ps) iova_t
}
class CacheBase~PTTag,PTData~ {
+lookup(tag, out_data, latency) bool
+fill(tag, data, from_prefetch) void
+invalidate_by_predicate(predicate) vector~Tag~
+invalidate(hash_tag, predicate, on_invalidated, latency) uint32_t
+invalidate_all_entries(latency) uint32_t
#hash_function(tag) uint32_t
#find_way(set, tag) int
#find_empty_way(set) int
}
class PTTag {
+gscid : uint16_t
+pscid : uint32_t
+iova : uint64_t
+stage : TransStage
+sv48 : bool
+gstage_x4 : bool
}
class PTData {
+vs_pte : spte_t
+g_pte : gpte_t
+reserved : pt_reserved_t
}
PTCache --|> CacheBase
PTCache --> PTTag : "使用"
PTCache --> PTData : "使用"
```

**图表来源**
- [pt_cache.h:8-72](file://iommu/cache_src/cache/pt_cache.h#L8-L72)
- [pt_cache.cpp:5-143](file://iommu/cache_src/cache/pt_cache.cpp#L5-L143)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)

**章节来源**
- [pt_cache.h:8-72](file://iommu/cache_src/cache/pt_cache.h#L8-L72)
- [pt_cache.cpp:5-143](file://iommu/cache_src/cache/pt_cache.cpp#L5-L143)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)

### 查找算法与命中/未命中处理
- 命中：基类 lookup 流程中，先计算 set，再在该 set 内顺序比较 tag，命中后更新替换策略计数并记录命中统计与延迟。
- **A/D位检查**：命中后检查A/D位状态，如果A/D未设置且SADE=1，则触发PTW进行硬件更新；如果A/D已设置或SADE=0，则直接转发。
- 未命中：记录缺失统计与延迟，返回 false，通常由上层触发页表遍历或 Walker Cache 查询。
- **占位CL处理**：在未命中时，检查去重缓冲区是否存在占位CL，如果存在则直接返回占位CL，避免重复PTW。
- 预取命中：若命中行来自预取，会额外统计预取命中并清除标记。

```mermaid
flowchart TD
Start(["进入 lookup"]) --> Hash["计算 set = hash(tag)"]
Hash --> FindWay["在 set 内查找匹配 tag 的 way"]
FindWay --> Hit{"找到 way ?"}
Hit --> |是| Update["更新替换策略/访问计数"]
Update --> CheckAD["检查A/D位状态"]
CheckAD --> NeedUpdate{"A/D需要更新且SADE=1 ?"}
NeedUpdate --> |是| TriggerPTW["触发PTW进行A/D更新"]
TriggerPTW --> ReturnPTW["返回PTW请求"]
NeedUpdate --> |否| DirectForward["直接转发到forwarder"]
DirectForward --> ReturnForward["返回转发"]
Hit --> |否| RecordMiss["记录缺失与执行延迟"]
RecordMiss --> DedupCheck["VA去重表检查"]
DedupCheck --> DedupHit{"去重命中 ?"}
DedupHit --> |是| HangWait["挂起任务，等待去重完成"]
HangWait --> ReturnWait["返回等待状态"]
DedupHit --> |否| PHCheck["占位CL/缓冲区检查"]
PHCheck --> PHHit{"占位CL存在 ?"}
PHHit --> |是| ReturnPH["返回占位CL"]
PHHit --> |否| StartWalk["启动页表遍历"]
StartWalk --> ReturnWalk["返回触发PTW"]
```

**图表来源**
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)

**章节来源**
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)

### 插入与更新策略
- 更新命中：若相同 tag 已存在于某 way，则直接更新数据与有效位，并更新替换策略。
- 空闲填充：若存在无效 way，则直接填入新 tag/data。
- 替换填充：若无空闲 way，则调用替换策略选择受害者 way，写入新数据并记录淘汰统计。
- 预取标记：fill 支持 from_prefetch 标记，便于统计预取命中率。
- **A/D位传递**：在更新PT Cache时，传递实际的A/D位状态，确保缓存数据与硬件状态一致。
- **占位CL插入**：insert_placeholder支持插入占位缓存行，标记为占位CL（is_ph=1），区分主任务占位（is_req=1）和预取占位（is_req=0）。
- **批量更新**：batch_update_placeholders将占位CL批量更新为常规CL，支持预取结果的快速填充。

```mermaid
sequenceDiagram
participant PT as "PTCache"
participant Base as "CacheBase"
participant Set as "Set"
participant RP as "替换策略"
PT->>Base : "fill(tag, data, from_prefetch)"
Base->>Base : "hash(tag)->set"
Base->>Set : "find_way(set, tag)"
alt "tag已存在"
Set-->>Base : "way索引"
Base->>Set : "更新数据/有效位/计数"
Base->>RP : "access(set, way)"
else "无空闲way"
Base->>Set : "find_empty_way(set)"
alt "有空闲"
Set-->>Base : "空闲way"
Base->>Set : "fill(tag,data,prefetch)"
else "无空闲"
Base->>RP : "find_victim(set)"
RP-->>Base : "victim_way"
Base->>Set : "替换写入并记录淘汰"
end
end
```

**图表来源**
- [cache_base.h:297-406](file://iommu/cache_src/cache/cache_base.h#L297-L406)
- [cache_base.h:551-558](file://iommu/cache_src/cache/cache_base.h#L551-L558)

**章节来源**
- [cache_base.h:297-406](file://iommu/cache_src/cache/cache_base.h#L297-L406)
- [cache_base.h:551-558](file://iommu/cache_src/cache/cache_base.h#L551-L558)

### 失效管理与级联失效
- VMA 失效：仅影响包含第一阶段的条目，支持精确/扫描/全局模式；精确模式下先计算 hash 定位 set 再比较 way。
- GVMA 失效：仅影响包含第二阶段的条目。
- 按上下文失效：按 gscid 或 gscid+pscid 扫描失效。
- 全局失效：清空所有有效行并重置替换状态。

```mermaid
flowchart TD
StartInv(["进入 invalidate_*"]) --> Mode{"模式"}
Mode --> |GLOBAL| InvalidateAll["遍历所有set/way, 清理并记录"]
Mode --> |PRECISE| PrecHash["计算hash定位set, 逐way比较"]
Mode --> |SCAN| ScanAll["逐set扫描, 逐way比较"]
PrecHash --> Match{"满足谓词?"}
ScanAll --> Match
Match --> |是| MarkInv["标记失效/记录"]
Match --> |否| NextWay["下一个way"]
MarkInv --> NextWay
NextWay --> Done["返回受影响条目数"]
```

**图表来源**
- [pt_cache.cpp:41-95](file://iommu/cache_src/cache/pt_cache.cpp#L41-L95)
- [cache_base.h:442-538](file://iommu/cache_src/cache/cache_base.h#L442-L538)

**章节来源**
- [pt_cache.cpp:41-95](file://iommu/cache_src/cache/pt_cache.cpp#L41-L95)
- [cache_base.h:442-538](file://iommu/cache_src/cache/cache_base.h#L442-L538)

### 数据结构与缓存行组织
- PTTag：包含 gscid、pscid、对齐后的 iova、翻译阶段(stage)、SV48 标志、G-stage X4 标志，用于唯一标识一次页表查询上下文。
- PTData：包含 VS 与 G 两阶段 PTE 的位域结构，以及 reserved 元信息，用于记录翻译类型、输入/输出页大小、IOVA 是否为 VA、SV48/X4 等。
- **新增PTReserved结构**：包含占位CL标志（is_ph）、主任务占位标志（is_req）、Buffer链表索引（head_index/tail_index）等。
- CacheLine：每个 set 内的多 way 缓存行，包含 valid、tag、data、from_prefetch、access_count 等字段。

```mermaid
erDiagram
PT_TAG {
uint16 gscid
uint32 pscid
uint64 iova
enum stage
bool sv48
bool gstage_x4
}
PT_DATA {
spte_t vs_pte
gpte_t g_pte
PT_RESERVED reserved
}
PT_RESERVED {
bool valid
uint32 trans_type
uint32 input_page_size
uint32 result_page_size
bool iova_is_va
bool sv48
bool gstage_x4
bool is_ph
uint8 head_index
uint8 tail_index
bool is_req
}
CACHE_LINE {
bool valid
PT_TAG tag
PT_DATA data
bool from_prefetch
uint64 access_count
}
PT_DATA ||--o{ CACHE_LINE : "存储于"
PT_TAG ||--o{ CACHE_LINE : "关联于"
```

**图表来源**
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [cache_line.h:70-91](file://iommu/cache_src/cache/cache_line.h#L70-L91)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)
- [types.h:375-414](file://iommu/cache_src/common/types.h#L375-L414)

**章节来源**
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [cache_line.h:70-91](file://iommu/cache_src/cache/cache_line.h#L70-L91)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)
- [types.h:375-414](file://iommu/cache_src/common/types.h#L375-L414)

### 性能特征与延迟建模
- 延迟来源：仲裁延迟 + 哈希/读 set/比较/写入等流水线延迟，分别对应查找命中/未命中、更新命中/空闲/替换三种路径。
- 统计指标：总访问、命中、缺失、替换、失效、预取命中/发出、平均执行/排队/请求延迟、IOPS 等。
- 命中率分析：仓库提供专门的命中率分析文档，可用于评估不同工作负载下的性能表现。
- **A/D位影响**：A/D位检查增加了额外的判断逻辑，但避免了不必要的PTW请求，整体性能影响可控。
- **占位CL影响**：占位CL机制减少了重复PTW，但需要额外的缓冲区管理和替换保护逻辑。

**章节来源**
- [cache_base.h:146-216](file://iommu/cache_src/cache/cache_base.h#L146-L216)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)

## 单线程调度器架构

### 统一轮询调度机制
**新增** 单线程调度器架构通过统一轮询调度机制实现任务级串行处理，避免传统多线程架构中的互锁死锁问题。该架构将原本分散在多个worker线程中的功能整合到单一调度器中：

- **pt_scheduler_thread**：合并pt_worker_thread和pt_update_worker_thread，实现REQUEST优先处理，UPDATE次之的任务级串行处理
- **walker_scheduler_thread**：按lookup → update → invalidate顺序轮询，避免互锁死锁问题
- **任务级串行**：确保同一时间只有一个任务在执行，避免竞争条件和死锁

```mermaid
sequenceDiagram
participant SC as "CacheSubsystem"
participant PT as "PT Cache"
participant FIFO as "请求FIFO"
SC->>SC : "pt_scheduler_thread启动"
loop "持续运行"
SC->>FIFO : "检查pt_request_fifo"
alt "有REQUEST请求"
SC->>SC : "读取请求并计算FIFO等待时间"
SC->>PT : "execute_pt_request(req)"
PT-->>SC : "返回响应"
SC->>SC : "记录相位时间戳"
SC->>SC : "trace_task_event"
SC->>SC : "push到pt_hit_response_fifo"
else "有UPDATE请求"
SC->>SC : "读取更新请求并计算等待时间"
SC->>PT : "execute_pt_update(req)"
PT-->>SC : "返回更新结果"
SC->>SC : "记录相位时间戳"
SC->>SC : "trace_task_event"
end
end
```

**图表来源**
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)

### REQUEST与UPDATE优先级处理
**新增** 统一轮询调度器实现了明确的优先级处理机制：

- **REQUEST优先级**：优先处理PT Cache的查找请求，确保地址转换的实时性
- **UPDATE次级优先级**：在没有REQUEST请求时处理UPDATE请求，保证缓存更新的及时性
- **FIFO等待时间统计**：通过accumulate_phase_task_wait记录任务在FIFO中的等待时间
- **相位时间戳记录**：通过record_pt_phase_timestamp记录REQUEST和UPDATE阶段的时间戳

**章节来源**
- [cache_subsystem.cpp:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [stats_collector.cpp:188-195](file://iommu/cache_src/common/stats_collector.cpp#L188-L195)
- [stats_collector.cpp:147-153](file://iommu/cache_src/common/stats_collector.cpp#L147-L153)

## 增强统计收集系统

### 相位时间戳记录
**新增** 增强的统计收集系统提供了更精确的性能分析能力：

- **record_pt_phase_timestamp**：记录PT Cache REQUEST和UPDATE阶段的开始和结束时间戳
- **accumulate_phase_task_wait**：统计任务在FIFO中的等待时间，区分REQUEST和UPDATE阶段
- **相位分离统计**：将排队等待时间和执行时间分离，提供更准确的性能分析

```mermaid
flowchart TD
Start(["任务开始"]) --> FIFOWait["FIFO等待时间统计"]
FIFOWait --> RequestPhase["REQUEST阶段执行"]
RequestPhase --> Timestamp1["记录REQUEST开始时间戳"]
Timestamp1 --> ExecuteRequest["执行查找操作"]
ExecuteRequest --> Timestamp2["记录REQUEST结束时间戳"]
Timestamp2 --> UpdatePhase["UPDATE阶段执行"]
UpdatePhase --> Timestamp3["记录UPDATE开始时间戳"]
Timestamp3 --> ExecuteUpdate["执行更新操作"]
ExecuteUpdate --> Timestamp4["记录UPDATE结束时间戳"]
Timestamp4 --> Complete["任务完成"]
```

**图表来源**
- [stats_collector.cpp:147-153](file://iommu/cache_src/common/stats_collector.cpp#L147-L153)
- [stats_collector.cpp:188-195](file://iommu/cache_src/common/stats_collector.cpp#L188-L195)
- [cache_subsystem.cpp:333-334](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L333-L334)
- [cache_subsystem.cpp:356-357](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L356-L357)

### 任务等待时间统计
**新增** 任务等待时间统计功能提供了更深入的系统分析能力：

- **FIFO等待时间**：统计任务在pt_request_fifo中的排队等待时间
- **相位分离**：区分REQUEST和UPDATE阶段的等待时间
- **精确测量**：通过sc_time_stamp()精确计算任务等待时间

**章节来源**
- [stats_collector.cpp:179-195](file://iommu/cache_src/common/stats_collector.cpp#L179-L195)
- [cache_subsystem.cpp:324-326](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L324-L326)
- [cache_subsystem.cpp:350-352](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L350-L352)

## PT Cache替换保护机制

### 占位CL保护策略修复
**修复** PT Cache替换保护机制bug，完善占位CL保护策略：

- **保护检测**：扫描所有way查找受保护的is_req=1占位CL
- **安全替换**：跳过受保护的way，寻找非保护的victim way
- **错误处理**：当所有way都受保护时，记录警告并跳过插入

```mermaid
flowchart TD
Start(["替换候选确定"]) --> CheckSuggested["检查建议的victim way"]
CheckSuggested --> Protected{"is_req=1占位CL ?"}
Protected --> |是| ScanAll["扫描所有way寻找安全victim"]
ScanAll --> FoundSafe{"找到非保护way ?"}
FoundSafe --> |是| UseSafe["使用安全victim way"]
FoundSafe --> |否| Warn["记录警告: Cache满, 无安全victim"]
Protected --> |否| UseSuggested["使用建议victim way"]
UseSafe --> Replace["执行替换"]
UseSuggested --> Replace
Warn --> Skip["跳过插入"]
Replace --> Complete["替换完成"]
Skip --> Complete
```

**图表来源**
- [cache_base.h:420-444](file://iommu/cache_src/cache/cache_base.h#L420-L444)

### 替换保护机制实现
**新增** 替换保护机制通过以下步骤实现：

- **受保护检查**：检查cache_array_[set][w].data.reserved.is_ph==1且is_req==1
- **扫描策略**：跳过已检查的suggested_way，扫描剩余way
- **安全选择**：选择第一个非保护的way作为victim
- **降级处理**：如果无安全victim，记录警告并跳过插入

**章节来源**
- [cache_base.h:420-444](file://iommu/cache_src/cache/cache_base.h#L420-L444)
- [TEST_50_PACKETS_D3_ANALYSIS_20260609.md](file://TEST_50_PACKETS_D3_ANALYSIS_20260609.md)

## 性能报告系统增强

### PT Cache性能摘要
**新增** 性能报告系统增强了PT Cache的性能分析能力：

- **性能摘要**：包含总任务数、REQUEST/UPDATE比例、PT Cache IOPS、RAM端口利用率等
- **任务放大系数**：计算RAM访问次数与原始任务数的比值
- **窗口分析**：基于first_access_time_ns和last_access_time_ns的窗口分析

```mermaid
flowchart TD
Start(["性能报告生成"]) --> WindowCalc["计算观察窗口"]
WindowCalc --> TaskCount["统计总任务数"]
TaskCount --> IOPS["计算PT Cache IOPS"]
IOPS --> Utilization["计算RAM端口利用率"]
Utilization --> Amplification["计算任务放大系数"]
Amplification --> Summary["生成性能摘要"]
Summary --> Complete["报告完成"]
```

**图表来源**
- [stats_collector.cpp:486-499](file://iommu/cache_src/common/stats_collector.cpp#L486-L499)
- [stats_collector.cpp:338-344](file://iommu/cache_src/common/stats_collector.cpp#L338-L344)

### 时间戳与任务放大分析
**新增** 时间戳记录功能提供了精确的性能分析基础：

- **相位时间戳**：记录REQUEST和UPDATE阶段的开始/结束时间
- **任务放大系数**：total_ram_accesses/original_tasks
- **执行时间分析**：基于total_execute的端口利用率计算

**章节来源**
- [stats_collector.cpp:337-344](file://iommu/cache_src/common/stats_collector.cpp#L337-L344)
- [stats_collector.cpp:486-499](file://iommu/cache_src/common/stats_collector.cpp#L486-L499)

## PT Cache优化改进

### 预取功能实现
**新增** PT Cache优化改进包含了完整的预取功能实现：

- **预取参数传递**：通过CacheMessage结构体传递prefetch_enabled和prefetch_depth
- **MISS时插入占位CL**：在未命中时插入D个预取占位CL，避免重复PTW
- **HIT预取占位CL处理**：处理is_req=0的预取占位CL，实现预取任务挂接
- **PTW Burst预取**：实现连续D个PTE的DDR读取和批量响应处理

```mermaid
sequenceDiagram
participant PTW as "PTW"
participant Cache as "PT Cache"
participant Buffer as "去重缓冲区"
PTW->>PTW : "检查预取参数"
alt "启用预取"
PTW->>Cache : "MISS时插入D个预取占位CL"
Cache->>Buffer : "记录Buffer链表信息"
PTW->>PTW : "发起Burst DDR读取"
PTW->>PTW : "解析D+1个PTE响应"
PTW->>Cache : "批量更新占位CL为常规CL"
Cache->>Buffer : "刷新Buffer链表"
Buffer-->>PTW : "批量转发任务"
else "禁用预取"
PTW->>PTW : "正常PTW流程"
end
```

**图表来源**
- [PT_DEDUP_PREFETCH_FINAL_SCHEME.md:757-813](file://PT_DEDUP_PREFETCH_FINAL_SCHEME.md#L757-L813)
- [PT_CACHE_PREFETCH_IMPLEMENTATION_ANALYSIS_20260609.md:345-372](file://PT_CACHE_PREFETCH_IMPLEMENTATION_ANALYSIS_20260609.md#L345-L372)

### Buffer链表管理
**新增** Buffer链表管理实现了高效的预取任务挂接：

- **Buffer Entry管理**：256个Buffer Entry的顺序分配和链表挂接
- **链表遍历**：通过next_index形成Buffer链表，支持批量处理
- **批量刷新**：flush_dedup_buffer_chain实现Buffer链表的批量处理和释放

**章节来源**
- [PT_DEDUP_PREFETCH_FINAL_SCHEME.md:604-664](file://PT_DEDUP_PREFETCH_FINAL_SCHEME.md#L604-L664)
- [dedup_buffer.h:13-122](file://iommu/cache_src/common/dedup_buffer.h#L13-L122)

### A/D位支持功能

#### A/D位检查机制
A/D位支持功能通过以下机制实现内存访问跟踪：

- **A/D位状态检查**：在PT Cache命中时，检查任务的A/D位状态（A=Accessed, D=Dirty）
- **更新条件判断**：如果A/D未设置且SADE=1（允许硬件更新），则触发PTW进行硬件更新
- **直接转发条件**：如果A/D已设置或SADE=0，则直接转发到forwarder，避免不必要的硬件更新

```mermaid
flowchart TD
Start(["PT Cache命中"]) --> CheckAD["检查A/D位状态"]
CheckAD --> NeedUpdate{"需要A/D更新 ?"}
NeedUpdate --> |A=0或(D=0且写访问)| CheckSADE{"SADE=1 ?"}
NeedUpdate --> |A=1且D=1| DirectForward["直接转发"]
CheckSADE --> |是| TriggerPTW["触发PTW进行A/D更新"]
CheckSADE --> |否| DirectForward
TriggerPTW --> ReturnPTW["返回PTW请求"]
DirectForward --> ReturnForward["返回转发"]
```

**图表来源**
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)

#### 动态A/D位传递
A/D位通过以下流程在系统中动态传递：

1. **从任务提取**：在`task_to_pt_update`中从任务结构提取实际的A/D位值
2. **构建PTData**：调用`make_pt_data`时传递A/D位状态
3. **缓存存储**：PT Cache存储完整的A/D位信息
4. **命中提取**：在`pt_hit_response_to_task`中从缓存提取A/D位到任务

```mermaid
sequenceDiagram
participant Task as "任务结构"
participant Convert as "任务转换"
participant PTData as "PTData"
participant Cache as "PT Cache"
Task->>Convert : "提取A/D位状态"
Convert->>PTData : "构建PTData(含A/D)"
PTData->>Cache : "写入PT Cache"
Cache-->>Convert : "命中返回(含A/D)"
Convert-->>Task : "提取A/D位到任务"
```

**图表来源**
- [iommu_task_cache_convert.cc:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-291)
- [iommu_task_cache_convert.cc:208-222](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L208-222)

#### A/D位处理流程
A/D位处理涉及多个组件的协作：

- **make_pt_data函数**：新增`ad_bit_set`参数，支持动态传递A/D位状态
- **task_to_pt_update函数**：从任务中提取实际A/D位值，传递给make_pt_data
- **pt_hit_response_to_task函数**：从缓存响应中提取A/D位到任务结构
- **PT Cache响应处理**：检查A/D位状态决定是否需要PTW更新

**章节来源**
- [types.h:370-423](file://iommu/cache_src/common/types.h#L370-L423)
- [iommu_task_cache_convert.cc:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-291)
- [iommu_task_cache_convert.cc:208-222](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L208-222)
- [iommu_perf_pt_cache_response.cc:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)

#### 日志增强功能
A/D位支持增强了系统的可观测性：

- **PT_UPDATE日志**：显示A/D位的实际值（A=1, D=1）
- **PT_CACHE响应日志**：显示A/D位状态和决策过程
- **调试信息**：提供详细的A/D位检查和更新信息

**章节来源**
- [iommu_task_cache_convert.cc:293-297](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L293-L297)
- [iommu_perf_pt_cache_response.cc:40-53](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L40-L53)

## VA去重功能

### deduplication table 结构
VA去重功能通过deduplication table实现，该表存储同页VA访问的去重信息：

- **va_dedup_key_t**：去重键值结构，包含 gscid、pscid 和 page_iova（4KB页对齐的IOVA）。
- **va_dedup_entry_t**：去重表项结构，包含：
  - valid：表项有效性标志
  - first_task_id：首个去重未命中的任务ID（去重MISS）
  - pending_tasks：等待去重完成的任务列表（去重HIT）
- **va_dedup_table**：std::map<va_dedup_key_t, va_dedup_entry_t>，存储所有活跃的去重条目。

```mermaid
classDiagram
class va_dedup_key_t {
+uint32_t gscid
+uint32_t pscid
+uint64_t page_iova
+operator<(other) bool
}
class va_dedup_entry_t {
+bool valid
+uint32_t first_task_id
+vector<iommu_task_t*> pending_tasks
}
class va_dedup_table {
+map<va_dedup_key_t, va_dedup_entry_t> table
+sc_mutex mutex
+uint64_t hit_count
+uint64_t miss_count
}
va_dedup_table --> va_dedup_key_t : "键"
va_dedup_table --> va_dedup_entry_t : "值"
```

**图表来源**
- [iommu_top.hh:123-142](file://iommu/iommu_top.hh#L123-L142)

### VA去重工作流程
VA去重功能在PT Cache未命中时激活，通过以下步骤实现：

1. **去重检查**：当PT Cache未命中时，检查va_dedup_table中是否存在相同的(gscid, pscid, page_iova)键
2. **去重命中**：如果存在有效条目，将当前任务挂入pending_tasks列表，不触发PTW
3. **去重未命中**：创建新的va_dedup_entry_t条目，记录first_task_id，触发PTW
4. **去重恢复**：当PTW完成后，从va_dedup_table中取出pending_tasks列表，批量转发给forwarder

```mermaid
sequenceDiagram
participant PT as "PT Cache"
participant Dedup as "VA去重表"
participant PTW as "页表遍历"
participant FWD as "Forwarder"
PT->>PT : "PT Cache未命中"
PT->>Dedup : "查找(gscid, pscid, page_iova)"
alt "去重命中"
Dedup-->>PT : "valid=true, pending_tasks存在"
PT->>PT : "挂起当前任务"
PT-->>PT : "等待去重完成"
else "去重未命中"
Dedup-->>PT : "不存在或invalid"
PT->>Dedup : "创建新条目，记录first_task_id"
PT->>PTW : "触发PTW"
PTW-->>Dedup : "PTW完成，保存结果"
Dedup->>FWD : "批量转发pending_tasks"
FWD-->>FWD : "逐个任务复制翻译结果"
end
```

**图表来源**
- [iommu_perf_pt_cache_response.cc:78-108](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L78-L108)
- [iommu_top.cc:477-529](file://iommu/iommu_top.cc#L477-L529)

### 同步机制与线程安全
VA去重功能采用严格的同步机制确保线程安全：

- **互斥锁**：va_dedup_mtx保护va_dedup_table的并发访问
- **原子操作**：va_dedup_hit_count和va_dedup_miss_count的更新需要加锁保护
- **RAII模式**：使用lock()/unlock()确保异常安全的资源管理

### 统计信息与性能分析
VA去重功能提供详细的统计信息：

- **va_dedup_hit_count**：去重命中次数（同页VA访问被去重）
- **va_dedup_miss_count**：去重未命中次数（首次访问或新页面）
- **PTW任务节省率**：va_dedup_hit_count/(va_dedup_hit_count+va_dedup_miss_count)×100%

这些统计信息通过print_cache_statistics()函数输出，帮助评估VA去重功能的性能收益。

**章节来源**
- [iommu_top.hh:123-142](file://iommu/iommu_top.hh#L123-L142)
- [iommu_perf_pt_cache_response.cc:78-108](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L78-L108)
- [iommu_top.cc:477-529](file://iommu/iommu_top.cc#L477-L529)
- [iommu_top.cc:547-556](file://iommu/iommu_top.cc#L547-L556)

## 占位CL与去重缓冲区

### 占位CL插入机制
占位CL（Placeholder Cache Line）机制通过以下接口实现：

- **insert_placeholder接口**：插入占位缓存行，支持主任务占位（is_req=1）和预取占位（is_req=0）
- **占位CL数据结构**：PTData.reserved字段包含is_ph=1、head_index、tail_index、is_req等标志
- **替换保护机制**：当所有way都被is_req=1的占位CL保护时，阻止新占位CL插入，触发回退逻辑

```mermaid
flowchart TD
Start(["insert_placeholder调用"]) --> BuildTag["构造PTTag(4KB页对齐)"]
BuildTag --> CheckFull["检查Cache是否全保护"]
CheckFull --> AllProtected{"所有way被is_req=1保护 ?"}
AllProtected --> |是| Fallback["触发回退: 直接PTW或丢弃"]
AllProtected --> |否| CreatePH["创建占位CL数据"]
CreatePH --> FillCache["调用基类fill插入"]
FillCache --> Success["插入成功"]
Fallback --> ReturnFalse["返回false"]
Success --> ReturnTrue["返回true"]
```

**图表来源**
- [pt_cache.cpp:149-235](file://iommu/cache_src/cache/pt_cache.cpp#L149-L235)

### 批量更新机制
批量更新功能通过batch_update_placeholders实现：

- **批量更新流程**：遍历更新列表，将占位CL更新为常规CL或新建常规CL
- **占位CL识别**：检查existing_data.reserved.is_ph标志判断是否为占位CL
- **更新策略**：
  - 占位CL→常规CL：设置is_ph=0，保留Buffer链信息
  - 未命中→新建：直接fill_pt创建常规CL
  - 常规CL→更新：直接fill覆盖

```mermaid
sequenceDiagram
participant PT as "PTCache"
participant Base as "CacheBase"
PT->>PT : "batch_update_placeholders"
loop "遍历updates"
PT->>PT : "lookup_pt检查占位CL"
alt "命中占位CL"
PT->>PT : "更新is_ph=0, 设置Buffer链"
PT->>Base : "fill覆盖占位CL为常规CL"
else "未命中"
PT->>Base : "fill_pt新建常规CL"
else "命中常规CL"
PT->>Base : "fill直接更新"
end
end
```

**图表来源**
- [pt_cache.cpp:237-295](file://iommu/cache_src/cache/pt_cache.cpp#L237-L295)

### 去重缓冲区管理
去重缓冲区（Dedup Buffer）通过以下组件实现：

- **DedupBuffer类**：管理256个Buffer Entry，支持顺序分配和链表挂接
- **BufferEntry结构**：包含gscid、pscid、iova、stage、sv48、gstage_x4、next_index、task_ptr等字段
- **链表管理**：通过next_index形成Buffer链表，支持任务挂起和批量恢复

```mermaid
classDiagram
class DedupBufferEntry {
+uint8_t valid
+uint8_t reserved1[3]
+gscid_t gscid
+pscid_t pscid
+iova_t iova
+TransStage stage
+bool sv48
+bool gstage_x4
+uint8_t next_index
+iommu_task_t* task_ptr
+bool is_valid()
+void clear()
}
class DedupBuffer {
+DedupBufferEntry entries[256]
+uint8_t valid_count
+uint8_t allocate_entry()
+void free_entry(uint8_t idx)
+bool is_full()
+uint8_t get_valid_count()
+void reset()
}
DedupBuffer --> DedupBufferEntry : "管理256个条目"
```

**图表来源**
- [dedup_buffer.h:13-122](file://iommu/cache_src/common/dedup_buffer.h#L13-L122)

### flush_dedup_buffer_chain机制
flush_dedup_buffer_chain负责去重缓冲区的批量处理：

- **链表遍历**：从head_index开始，沿next_index遍历整个Buffer链
- **批量恢复**：将链表中的所有任务批量转发给forwarder
- **Buffer释放**：释放链表中所有Buffer Entry，重置状态

**章节来源**
- [pt_cache.h:48-59](file://iommu/cache_src/cache/pt_cache.h#L48-L59)
- [pt_cache.cpp:149-295](file://iommu/cache_src/cache/pt_cache.cpp#L149-L295)
- [dedup_buffer.h:13-122](file://iommu/cache_src/common/dedup_buffer.h#L13-L122)
- [iommu_top.hh:146-167](file://iommu/iommu_top.hh#L146-L167)

## 依赖关系分析
- PTCache 依赖 CacheBase 提供的通用缓存能力；CacheBase 再依赖替换策略、统计收集器与配置。
- **新增CacheSubsystem**：单线程调度器架构，通过统一轮询调度机制实现任务级串行处理。
- **新增StatsCollector**：增强统计收集器，提供相位时间戳记录和任务等待时间统计。
- PTTag/PTData 依赖公共类型定义，如 TransStage、PageSize、spte_t/gpte_t 等。
- 配置通过 JSON 解析注入到缓存子系统，最终传入 PTCache 构造函数。
- **A/D位支持**：依赖iommu_task.hh中的SADE字段和iommu_perf_params.hh中的配置。
- **VA去重功能**：依赖iommu_perf_params.hh中的PT_CACHE_VA_DEDUP_ENABLED配置，通过iommu_top.hh中的数据结构实现。
- **占位CL与去重缓冲区**：依赖dedup_buffer.h中的DedupBuffer类，通过iommu_top.hh中的flush_dedup_buffer_chain实现。

```mermaid
graph LR
PTCache["PTCache"] --> CacheBase["CacheBase<T,D>"]
PTCache --> Stats["StatsCollector"]
CacheBase --> Replacement["ReplacementPolicy"]
CacheBase --> Stats
CacheSubsystem["CacheSubsystem"] --> PTCache
CacheSubsystem --> Stats
JSON["json_config.cpp"] --> Subsys["cache_subsystem.cpp"]
Subsys --> PTCache
Params["iommu_perf_params.hh"] --> Dedup["VA去重功能"]
Dedup --> Top["iommu_top.hh/.cc"]
ADSupport["A/D位支持"] --> Task["iommu_task.hh"]
ADSupport --> Convert["task_cache_convert.cc"]
ADSupport --> Response["pt_cache_response.cc"]
PHSupport["占位CL支持"] --> PHInterface["pt_cache.h/.cpp"]
PHSupport --> DB["dedup_buffer.h"]
PHSupport --> PF["flush_dedup_buffer_chain"]
```

**图表来源**
- [pt_cache.cpp:5-8](file://iommu/cache_src/cache/pt_cache.cpp#L5-L8)
- [cache_base.h:235-252](file://iommu/cache_src/cache/cache_base.h#L235-L252)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [types.h:65-69](file://iommu/cache_src/common/types.h#L65-L69)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [iommu_perf_params.hh:113-114](file://iommu/iommu_perf_model/iommu_perf_params.hh#L113-L114)
- [iommu_task.hh:160-162](file://iommu/include/iommu_task.hh#L160-L162)
- [dedup_buffer.h:1-122](file://iommu/cache_src/common/dedup_buffer.h#L1-L122)

**章节来源**
- [pt_cache.cpp:5-8](file://iommu/cache_src/cache/pt_cache.cpp#L5-L8)
- [cache_base.h:235-252](file://iommu/cache_src/cache/cache_base.h#L235-L252)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [types.h:65-69](file://iommu/cache_src/common/types.h#L65-L69)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [iommu_perf_params.hh:113-114](file://iommu/iommu_perf_model/iommu_perf_params.hh#L113-L114)
- [iommu_task.hh:160-162](file://iommu/include/iommu_task.hh#L160-L162)
- [dedup_buffer.h:1-122](file://iommu/cache_src/common/dedup_buffer.h#L1-L122)

## 性能考量
- 命中率优化
  - 合理设置 num_sets 与 num_ways：增大 ways 可降低替换开销，增大 sets 可降低冲突。
  - 选择替换策略：SRRIP 在高并发场景表现稳定；PLRU 更简单且适合低开销需求。
  - 预取策略：利用 from_prefetch 标记，结合命中统计评估预取收益。
  - **VA去重优化**：对于具有高空间局部性的工作负载，VA去重可显著减少PTW任务数量。
  - **占位CL优化**：合理配置预取深度D，平衡预取收益与Cache占用。
  - **单线程调度优化**：统一轮询调度避免互锁死锁，提高系统稳定性。
- 内存占用
  - 每个缓存行包含 tag + data + 控制位，内存占用约为 (tag_size + data_size + 1字节控制位) × sets × ways。
  - PTData 包含 VS/G 两阶段 PTE 与 reserved 字段，占用相对较大，需权衡精度与容量。
  - **VA去重表内存**：每个去重表项约占用24字节（1+4+8+8+4），实际占用取决于活跃页面数量。
  - **去重缓冲区内存**：256个Buffer Entry，每个Entry约占用20字节，总占用约5KB。
  - **A/D位存储**：每个PTData额外存储A/D位状态，增加约2位存储开销。
  - **统计信息内存**：相位时间戳和任务等待时间统计增加少量内存开销。
- 延迟优化
  - 减少仲裁等待：合理设置 arbiter_latency_cycles 与流水线延迟参数。
  - 降低替换开销：在替换路径较长时，考虑增大 sets 或采用更高效的替换策略。
  - **VA去重延迟**：去重命中可避免PTW延迟，但需要额外的去重表查找和同步开销。
  - **A/D位检查延迟**：增加的A/D位检查逻辑带来微小延迟，但避免了不必要的PTW请求。
  - **占位CL延迟**：占位CL机制可减少重复PTW，但需要额外的缓冲区管理和替换保护逻辑。
  - **单线程调度延迟**：统一轮询调度引入轻微的调度开销，但避免了复杂的同步机制。
- 命中率分析
  - 参考仓库提供的命中率分析文档，结合工作负载特征调整缓存规模与替换策略。
  - **VA去重效果评估**：通过统计信息分析去重命中率和PTW任务节省率。
  - **占位CL效果评估**：通过PT_CACHE_ACCESS_ANALYSIS文档评估占位CL的命中模式和预取效果。
  - **A/D位影响分析**：评估A/D位检查对整体性能的影响，通常为正向优化。
  - **统计系统效果评估**：通过相位时间戳和任务等待时间统计分析系统性能瓶颈。

**章节来源**
- [types.h:573-589](file://iommu/cache_src/common/types.h#L573-L589)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)
- [iommu_top.cc:547-556](file://iommu/iommu_top.cc#L547-L556)
- [PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md](file://PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md)

## 故障排查指南
- 命中率异常偏低
  - 检查 num_sets 是否过小导致高冲突；检查 pagesize 对齐是否正确。
  - 关注预取命中统计，评估预取策略有效性。
  - **VA去重问题**：检查va_dedup_table大小是否过大导致内存压力。
  - **A/D位问题**：检查A/D位状态是否正确传递，确认SADE配置。
  - **占位CL问题**：检查insert_placeholder返回值，确认占位CL插入是否成功。
  - **单线程调度问题**：检查pt_scheduler_thread是否正常运行，确认REQUEST优先级处理。
- 失效不生效
  - 确认失效模式（PRECISE/SCAN/GLOBAL）与谓词条件是否匹配预期。
  - 检查 stage 标志（STAGE1_ONLY/STAGE2_ONLY/STAGE1_AND_2）是否与失效目标一致。
- 性能退化
  - 查看替换与失效统计，确认是否存在频繁替换或大量失效。
  - 分析延迟直方图，定位瓶颈在仲裁、哈希、比较还是写入阶段。
  - **VA去重相关问题**：
    - 去重命中率异常：检查va_dedup_hit_count和va_dedup_miss_count的平衡
    - 去重表内存泄漏：确认va_dedup_table的清理机制正常工作
    - 线程死锁：检查va_dedup_mtx的加锁/解锁配对是否正确
  - **A/D位相关问题**：
    - A/D更新频繁：检查SADE配置和访问模式
    - A/D位不正确：验证make_pt_data函数的A/D位传递逻辑
    - 性能下降：确认A/D位检查逻辑的优化效果
  - **占位CL相关问题**：
    - 占位CL插入失败：检查Cache是否被is_req=1的占位CL完全保护
    - 预取未生效：检查batch_update_placeholders是否正确执行
    - Buffer满：检查DedupBuffer的有效计数，确认是否达到256上限
  - **单线程调度相关问题**：
    - 调度器停止：检查pt_scheduler_thread的运行状态
    - 优先级错误：确认REQUEST请求是否总是优先于UPDATE请求
    - FIFO积压：检查pt_request_fifo和pt_update_fifo的队列长度
  - **统计系统相关问题**：
    - 时间戳丢失：检查record_pt_phase_timestamp的调用频率
    - 等待时间统计异常：确认accumulate_phase_task_wait的计算逻辑
    - 报告不完整：检查print_summary的输出格式和数据完整性

**章节来源**
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [pt_cache.cpp:41-95](file://iommu/cache_src/cache/pt_cache.cpp#L41-L95)
- [cache_base.h:442-538](file://iommu/cache_src/cache/cache_base.h#L442-L538)
- [iommu_top.cc:547-556](file://iommu/iommu_top.cc#L547-L556)
- [PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md](file://PT_CACHE_ACCESS_ANALYSIS_50TASKS_20260609.md)

## 结论
PT Cache 通过模板化的 CacheBase 提供了统一、可扩展的缓存框架，PTCache 在其中承担 IOTLB 的核心职责。**新增的单线程调度器架构通过统一轮询调度机制实现了任务级串行处理，避免了传统多线程架构中的互锁死锁问题，提高了系统的稳定性和可预测性。** **增强的统计收集系统提供了更精确的性能分析能力，通过相位时间戳记录和任务等待时间统计，为性能优化提供了数据支撑。** **修复的PT Cache替换保护机制完善了占位CL的保护策略，解决了Cache满时的替换问题，提高了系统的可靠性。** **性能报告系统的增强提供了更全面的性能摘要和任务放大系数分析，帮助用户更好地理解系统性能特征。** **新增的PT Cache优化改进包含了完整的预取功能实现，通过占位CL机制和Buffer链表管理，显著提升了预取效率和系统吞吐。** **新增的A/D位支持功能进一步完善了PT Cache的内存访问跟踪能力，通过A/D位检查机制实现了更精确的访问监控和硬件更新控制。** **新增的VA去重功能进一步提升了性能，通过deduplication table实现了同页VA访问的去重优化，显著减少了重复的PTW任务。** **新增的占位CL与去重缓冲区功能实现了预取优化和任务挂起管理，通过占位缓存行和Buffer链表机制，有效减少了重复的页表遍历操作。**通过合理的配置与替换策略选择，可在不同工作负载下获得稳定的命中率与较低的地址转换延迟。

## 附录

### 配置参数与默认值
- 缓存规模与替换
  - num_sets：set 数
  - num_ways：way 数
  - replacement：替换策略（"srrip"/"plru"/"none"）
  - srrip_m_bits：SRRIP M 位参数
- 仲裁与流水线延迟
  - arbiter_latency_cycles：仲裁阶段延迟
  - hash_latency_cycles：哈希计算延迟
  - read_set_latency_cycles：读取 set 延迟
  - compare_latency_cycles：比较每个 way 延迟
  - fill_compute_index_*：更新路径延迟（命中/空闲/替换）
  - write_way_latency_cycles：写入 way 延迟
  - invalidation_compare_per_way_cycles：失效比较每个 way 延迟
- 全局配置
  - clock_period_ns：时钟周期（ns）
  - random_seed：随机种子
  - pt_cache：PT Cache 的 CacheConfig
  - statistics：统计配置（输出文件、直方图等）
- **VA去重配置**
  - PT_CACHE_VA_DEDUP_ENABLED：VA去重功能开关（true/false）
- **A/D位配置**
  - SADE：硬件A/D更新允许标志（在iommu_task.hh中定义）
  - GADE：G-stage硬件A/D更新允许标志（在iommu_task.hh中定义）
- **占位CL配置**
  - PT_DEDUP_BUFFER_SIZE：去重缓冲区大小（默认256）
  - prefetch_depth：预取深度D值
- **单线程调度配置**
  - pt_scheduler_thread：统一轮询调度线程
  - walker_scheduler_thread：Walker Cache调度线程
- **统计系统配置**
  - record_pt_phase_timestamp：相位时间戳记录
  - accumulate_phase_task_wait：任务等待时间统计

**章节来源**
- [types.h:573-612](file://iommu/cache_src/common/types.h#L573-L612)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [iommu_perf_params.hh:113-114](file://iommu/iommu_perf_model/iommu_perf_params.hh#L113-L114)
- [iommu_task.hh:160-162](file://iommu/include/iommu_task.hh#L160-L162)
- [dedup_buffer.h:60-61](file://iommu/cache_src/common/dedup_buffer.h#L60-L61)
- [cache_subsystem.h:105-131](file://iommu/cache_src/subsystem/cache_subsystem.h#L105-L131)
- [stats_collector.cpp:147-195](file://iommu/cache_src/common/stats_collector.cpp#L147-L195)

### 代码示例路径（不含具体代码内容）
- 查找页表项
  - [lookup_pt 接口:18-24](file://iommu/cache_src/cache/pt_cache.h#L18-L24)
  - [lookup 实现:11-23](file://iommu/cache_src/cache/pt_cache.cpp#L11-L23)
  - [基类 lookup 流程:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- 插入/更新页表项
  - [fill_pt 接口:26-27](file://iommu/cache_src/cache/pt_cache.h#L26-L27)
  - [fill 实现:25-39](file://iommu/cache_src/cache/pt_cache.cpp#L25-L39)
  - [基类 fill 流程:297-406](file://iommu/cache_src/cache/cache_base.h#L297-L406)
- 失效操作
  - [VMA 失效接口:31-34](file://iommu/cache_src/cache/pt_cache.h#L31-L34)
  - [GVMA 失效接口:37-40](file://iommu/cache_src/cache/pt_cache.h#L37-L40)
  - [VMA 失效实现:41-74](file://iommu/cache_src/cache/pt_cache.cpp#L41-L74)
  - [GVMA 失效实现:76-95](file://iommu/cache_src/cache/pt_cache.cpp#L76-L95)
  - [按上下文失效实现:97-118](file://iommu/cache_src/cache/pt_cache.cpp#L97-L118)
  - [基类失效扫描/精确/全局:442-538](file://iommu/cache_src/cache/cache_base.h#L442-L538)
- 哈希与页对齐
  - [哈希函数:120-133](file://iommu/cache_src/cache/pt_cache.cpp#L120-L133)
  - [IOVA 页对齐:135-143](file://iommu/cache_src/cache/pt_cache.cpp#L135-L143)
- 统计与性能
  - [统计收集器接口:65-109](file://iommu/cache_src/common/stats_collector.h#L65-L109)
  - [命中/缺失/替换/失效统计:73-78](file://iommu/cache_src/common/stats_collector.h#L73-L78)
  - [命中率与IOPS计算:31-62](file://iommu/cache_src/common/stats_collector.h#L31-L62)
  - [相位时间戳记录:147-153](file://iommu/cache_src/common/stats_collector.cpp#L147-L153)
  - [任务等待时间统计:188-195](file://iommu/cache_src/common/stats_collector.cpp#L188-L195)
- **单线程调度器**
  - [pt_scheduler_thread:314-370](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L314-L370)
  - [walker_scheduler_thread:373-396](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L373-L396)
- **A/D位支持功能**
  - [make_pt_data函数:370-423](file://iommu/cache_src/common/types.h#L370-L423)
  - [task_to_pt_update函数:276-291](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L276-L291)
  - [pt_hit_response_to_task函数:208-222](file://iommu/iommu_perf_model/iommu_task_cache_convert.cc#L208-L222)
  - [PT Cache响应检查:36-54](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L36-L54)
- **VA去重功能**
  - [去重键值结构:123-133](file://iommu/iommu_top.hh#L123-L133)
  - [去重表项结构:134-138](file://iommu/iommu_top.hh#L134-L138)
  - [去重检查实现:78-108](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L78-L108)
  - [去重恢复实现:477-529](file://iommu/iommu_top.cc#L477-L529)
  - [统计输出:547-556](file://iommu/iommu_top.cc#L547-L556)
- **占位CL与去重缓冲区功能**
  - [insert_placeholder接口:48-54](file://iommu/cache_src/cache/pt_cache.h#L48-L54)
  - [batch_update_placeholders接口:56-59](file://iommu/cache_src/cache/pt_cache.h#L56-L59)
  - [占位CL插入实现:149-235](file://iommu/cache_src/cache/pt_cache.cpp#L149-L235)
  - [批量更新实现:237-295](file://iommu/cache_src/cache/pt_cache.cpp#L237-L295)
  - [DedupBuffer类:60-118](file://iommu/cache_src/common/dedup_buffer.h#L60-L118)
  - [flush_dedup_buffer_chain:150-157](file://iommu/iommu_top.hh#L150-L157)
- 性能模型集成
  - [PT Cache 响应线程:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)