# 页表缓存(PT Cache)

<cite>
**本文引用的文件**
- [pt_cache.h](file://iommu/cache_src/cache/pt_cache.h)
- [pt_cache.cpp](file://iommu/cache_src/cache/pt_cache.cpp)
- [cache_base.h](file://iommu/cache_src/cache/cache_base.h)
- [cache_base.cpp](file://iommu/cache_src/cache/cache_base.cpp)
- [cache_line.h](file://iommu/cache_src/cache/cache_line.h)
- [types.h](file://iommu/cache_src/common/types.h)
- [stats_collector.h](file://iommu/cache_src/common/stats_collector.h)
- [iommu_data_structures.hh](file://iommu/include/iommu_data_structures.hh)
- [json_config.cpp](file://iommu/cache_src/common/json_config.cpp)
- [cache_subsystem.cpp](file://iommu/cache_src/subsystem/cache_subsystem.cpp)
- [iommu_perf_pt_cache_response.cc](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)
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
本文件针对 RISC-V IOMMU 的页表缓存(PT Cache)进行系统化技术文档化，重点阐述其作为 IOTLB(IOMMU Page Table Lookaside Buffer)的核心作用：缓存页表项并加速地址转换。文档覆盖以下方面：
- PT Cache 的数据结构设计与缓存行组织方式
- 查找、插入与更新的处理流程与命中/未命中机制
- 失效管理策略（VMA/GVMA/按上下文批量失效）
- 性能特征、命中率优化与内存占用评估
- 配置参数、替换策略选择与调优建议
- 通过代码路径引用展示关键操作的实现位置

## 项目结构
PT Cache 位于 iommu/cache_src/cache 子目录，采用模板化基类 CacheBase 提供统一的缓存接口与性能建模，PTCache 作为特化实现，结合公共类型定义与统计收集器完成端到端的功能。

```mermaid
graph TB
subgraph "缓存实现"
PT["PTCache<br/>pt_cache.cpp/.h"]
CB["CacheBase<Tag,Data><br/>cache_base.h/.cpp"]
CL["CacheLine<br/>cache_line.h"]
end
subgraph "公共类型与配置"
TY["类型与数据结构<br/>types.h"]
JC["JSON配置解析<br/>json_config.cpp"]
SS["缓存子系统集成<br/>cache_subsystem.cpp"]
end
subgraph "性能与统计"
ST["统计收集器<br/>stats_collector.h"]
PM["性能模型响应<br/>iommu_perf_pt_cache_response.cc"]
end
subgraph "IOMMU规范类型"
DS["数据结构定义<br/>iommu_data_structures.hh"]
end
PT --> CB
PT --> CL
PT --> TY
CB --> ST
SS --> PT
JC --> SS
PM --> PT
TY --> DS
```

**图表来源**
- [pt_cache.h:1-59](file://iommu/cache_src/cache/pt_cache.h#L1-L59)
- [pt_cache.cpp:1-146](file://iommu/cache_src/cache/pt_cache.cpp#L1-L146)
- [cache_base.h:1-676](file://iommu/cache_src/cache/cache_base.h#L1-L676)
- [cache_line.h:1-103](file://iommu/cache_src/cache/cache_line.h#L1-L103)
- [types.h:1-617](file://iommu/cache_src/common/types.h#L1-L617)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [stats_collector.h:1-130](file://iommu/cache_src/common/stats_collector.h#L1-L130)
- [iommu_data_structures.hh:1-200](file://iommu/include/iommu_data_structures.hh#L1-L200)
- [iommu_perf_pt_cache_response.cc:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)

**章节来源**
- [pt_cache.h:1-59](file://iommu/cache_src/cache/pt_cache.h#L1-L59)
- [pt_cache.cpp:1-146](file://iommu/cache_src/cache/pt_cache.cpp#L1-L146)
- [cache_base.h:1-676](file://iommu/cache_src/cache/cache_base.h#L1-L676)
- [cache_line.h:1-103](file://iommu/cache_src/cache/cache_line.h#L1-L103)
- [types.h:1-617](file://iommu/cache_src/common/types.h#L1-L617)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)
- [stats_collector.h:1-130](file://iommu/cache_src/common/stats_collector.h#L1-L130)
- [iommu_data_structures.hh:1-200](file://iommu/include/iommu_data_structures.hh#L1-L200)
- [iommu_perf_pt_cache_response.cc:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)

## 核心组件
- PTCache：PT Cache 的具体实现，继承自 CacheBase，提供页表项查找、填充与多种失效策略。
- CacheBase：通用缓存模板基类，封装哈希定位、set 内 way 比较、替换算法、仲裁与延迟建模等。
- CacheLine：缓存行模板，包含 valid 标志、tag、data、预取标记与访问计数。
- PTTag/PTData：PT Cache 的标签与数据结构，承载 gscid、pscid、IOVA、翻译阶段、SV48/X4 模式等关键信息。
- CacheConfig：缓存配置，包含 set 数、way 数、替换策略、仲裁与流水线延迟等。
- StatsCollector：统计收集器，记录访问、命中、缺失、替换、失效、队列延迟等指标。

**章节来源**
- [pt_cache.h:8-54](file://iommu/cache_src/cache/pt_cache.h#L8-L54)
- [pt_cache.cpp:5-39](file://iommu/cache_src/cache/pt_cache.cpp#L5-L39)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:70-91](file://iommu/cache_src/cache/cache_line.h#L70-L91)
- [types.h:33-45](file://iommu/cache_src/common/types.h#L33-L45)
- [types.h:573-589](file://iommu/cache_src/common/types.h#L573-L589)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)

## 架构总览
PT Cache 在 IOMMU 地址转换路径中扮演 IOTLB 角色，通过缓存页表项减少对主存页表的访问次数，提升整体吞吐。其核心流程：
- 查找：根据 gscid/pscid/iova/阶段/模式构造 PTTag，经哈希定位 set，遍历 way 完成 tag 比较，命中则返回 PTData 并更新替换策略。
- 填充：将 Walker 或页表遍历结果写入缓存，同时记录翻译阶段、输入/输出页大小、SV48/X4 等元信息。
- 失效：支持 VMA(G-stage)、GVMA(S-stage)、按 gscid/pscid 批量失效以及全局失效，按模式精确或扫描执行。

```mermaid
sequenceDiagram
participant Req as "请求方"
participant PT as "PTCache"
participant Base as "CacheBase"
participant Set as "Set(多way)"
participant RP as "替换策略"
Req->>PT : "lookup_pt(gscid, pscid, iova, stage, sv48, gstage_x4)"
PT->>PT : "构造PTTag并页对齐"
PT->>Base : "lookup(tag, out_data, latency)"
Base->>Base : "hash(tag)->set"
Base->>Set : "find_way(set, tag)"
alt "命中"
Set-->>Base : "way索引"
Base->>RP : "access(set, way)"
Base-->>PT : "true, out_data"
PT-->>Req : "返回命中数据"
else "未命中"
Set-->>Base : "未找到"
Base-->>PT : "false"
PT-->>Req : "未命中(触发页表遍历)"
end
```

**图表来源**
- [pt_cache.cpp:11-23](file://iommu/cache_src/cache/pt_cache.cpp#L11-L23)
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)

**章节来源**
- [pt_cache.cpp:11-23](file://iommu/cache_src/cache/pt_cache.cpp#L11-L23)
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)

## 详细组件分析

### PTCache 类与接口
- 查找接口：支持带 SV48/X4 参数的查找重载，内部统一构造 PTTag 并调用基类 lookup。
- 填充接口：将 Walker 结果或页表遍历结果写入缓存，自动设置翻译阶段、IOVA 是否为 VA、SV48/X4 等元信息。
- 失效接口：
  - VMA 失效：影响第一阶段翻译，支持精确/扫描/全局模式。
  - GVMA 失效：影响第二阶段翻译。
  - 按上下文失效：按 gscid/pscid 批量失效。
  - 全局失效：清空所有有效行。
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
- [pt_cache.h:8-54](file://iommu/cache_src/cache/pt_cache.h#L8-L54)
- [pt_cache.cpp:5-143](file://iommu/cache_src/cache/pt_cache.cpp#L5-L143)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)

**章节来源**
- [pt_cache.h:8-54](file://iommu/cache_src/cache/pt_cache.h#L8-L54)
- [pt_cache.cpp:5-143](file://iommu/cache_src/cache/pt_cache.cpp#L5-L143)
- [cache_base.h:26-256](file://iommu/cache_src/cache/cache_base.h#L26-L256)
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)

### 查找算法与命中/未命中处理
- 命中：基类 lookup 流程中，先计算 set，再在该 set 内顺序比较 tag，命中后更新替换策略计数并记录命中统计与延迟。
- 未命中：记录缺失统计与延迟，返回 false，通常由上层触发页表遍历或 Walker Cache 查询。
- 预取命中：若命中行来自预取，会额外统计预取命中并清除标记。

```mermaid
flowchart TD
Start(["进入 lookup"]) --> Hash["计算 set = hash(tag)"]
Hash --> FindWay["在 set 内查找匹配 tag 的 way"]
FindWay --> Hit{"找到 way ?"}
Hit --> |是| Update["更新替换策略/访问计数"]
Update --> RecordHit["记录命中与执行延迟"]
RecordHit --> ReturnTrue["返回 true, out_data"]
Hit --> |否| RecordMiss["记录缺失与执行延迟"]
RecordMiss --> ReturnFalse["返回 false"]
```

**图表来源**
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)

**章节来源**
- [cache_base.h:258-295](file://iommu/cache_src/cache/cache_base.h#L258-L295)
- [cache_base.h:541-548](file://iommu/cache_src/cache/cache_base.h#L541-L548)

### 插入与更新策略
- 更新命中：若相同 tag 已存在于某 way，则直接更新数据与有效位，并更新替换策略。
- 空闲填充：若存在无效 way，则直接填入新 tag/data。
- 替换填充：若无空闲 way，则调用替换策略选择受害者 way，写入新数据并记录淘汰统计。
- 预取标记：fill 支持 from_prefetch 标记，便于统计预取命中率。

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
pt_reserved_t reserved
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

**章节来源**
- [cache_line.h:33-45](file://iommu/cache_src/cache/cache_line.h#L33-L45)
- [cache_line.h:70-91](file://iommu/cache_src/cache/cache_line.h#L70-L91)
- [types.h:214-235](file://iommu/cache_src/common/types.h#L214-L235)

### 性能特征与延迟建模
- 延迟来源：仲裁延迟 + 哈希/读 set/比较/写入等流水线延迟，分别对应查找命中/未命中、更新命中/空闲/替换三种路径。
- 统计指标：总访问、命中、缺失、替换、失效、预取命中/发出、平均执行/排队/请求延迟、IOPS 等。
- 命中率分析：仓库提供专门的命中率分析文档，可用于评估不同工作负载下的性能表现。

**章节来源**
- [cache_base.h:146-216](file://iommu/cache_src/cache/cache_base.h#L146-L216)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)

## 依赖关系分析
- PTCache 依赖 CacheBase 提供的通用缓存能力；CacheBase 再依赖替换策略、统计收集器与配置。
- PTTag/PTData 依赖公共类型定义，如 TransStage、PageSize、spte_t/gpte_t 等。
- 配置通过 JSON 解析注入到缓存子系统，最终传入 PTCache 构造函数。

```mermaid
graph LR
PTCache["PTCache"] --> CacheBase["CacheBase<T,D>"]
CacheBase --> Replacement["ReplacementPolicy"]
CacheBase --> Stats["StatsCollector"]
PTCache --> Types["types.h"]
Types --> DS["iommu_data_structures.hh"]
JSON["json_config.cpp"] --> Subsys["cache_subsystem.cpp"]
Subsys --> PTCache
```

**图表来源**
- [pt_cache.cpp:5-8](file://iommu/cache_src/cache/pt_cache.cpp#L5-L8)
- [cache_base.h:235-252](file://iommu/cache_src/cache/cache_base.h#L235-L252)
- [types.h:65-69](file://iommu/cache_src/common/types.h#L65-L69)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)

**章节来源**
- [pt_cache.cpp:5-8](file://iommu/cache_src/cache/pt_cache.cpp#L5-L8)
- [cache_base.h:235-252](file://iommu/cache_src/cache/cache_base.h#L235-L252)
- [types.h:65-69](file://iommu/cache_src/common/types.h#L65-L69)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)
- [cache_subsystem.cpp:143-143](file://iommu/cache_src/subsystem/cache_subsystem.cpp#L143-L143)

## 性能考量
- 命中率优化
  - 合理设置 num_sets 与 num_ways：增大 ways 可降低替换开销，增大 sets 可降低冲突。
  - 选择替换策略：SRRIP 在高并发场景表现稳定；PLRU 更简单且适合低开销需求。
  - 预取策略：利用 from_prefetch 标记，结合命中统计评估预取收益。
- 内存占用
  - 每个缓存行包含 tag + data + 控制位，内存占用约为 (tag_size + data_size + 1字节控制位) × sets × ways。
  - PTData 包含 VS/G 两阶段 PTE 与 reserved 字段，占用相对较大，需权衡精度与容量。
- 延迟优化
  - 减少仲裁等待：合理设置 arbiter_latency_cycles 与流水线延迟参数。
  - 降低替换开销：在替换路径较长时，考虑增大 sets 或采用更高效的替换策略。
- 命中率分析
  - 参考仓库提供的命中率分析文档，结合工作负载特征调整缓存规模与替换策略。

**章节来源**
- [types.h:573-589](file://iommu/cache_src/common/types.h#L573-L589)
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [PT_CACHE_HIT_RATE_ANALYSIS.md](file://PT_CACHE_HIT_RATE_ANALYSIS.md)

## 故障排查指南
- 命中率异常偏低
  - 检查 num_sets 是否过小导致高冲突；检查 pagesize 对齐是否正确。
  - 关注预取命中统计，评估预取策略有效性。
- 失效不生效
  - 确认失效模式（PRECISE/SCAN/GLOBAL）与谓词条件是否匹配预期。
  - 检查 stage 标志（STAGE1_ONLY/STAGE2_ONLY/STAGE1_AND_2）是否与失效目标一致。
- 性能退化
  - 查看替换与失效统计，确认是否存在频繁替换或大量失效。
  - 分析延迟直方图，定位瓶颈在仲裁、哈希、比较还是写入阶段。

**章节来源**
- [stats_collector.h:13-63](file://iommu/cache_src/common/stats_collector.h#L13-L63)
- [pt_cache.cpp:41-95](file://iommu/cache_src/cache/pt_cache.cpp#L41-L95)
- [cache_base.h:442-538](file://iommu/cache_src/cache/cache_base.h#L442-L538)

## 结论
PT Cache 通过模板化的 CacheBase 提供了统一、可扩展的缓存框架，PTCache 在其中承担 IOTLB 的核心职责。其设计兼顾了查找效率、替换策略灵活性与性能统计可观测性。通过合理的配置与替换策略选择，可在不同工作负载下获得稳定的命中率与较低的地址转换延迟。

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

**章节来源**
- [types.h:573-612](file://iommu/cache_src/common/types.h#L573-L612)
- [json_config.cpp:63-74](file://iommu/cache_src/common/json_config.cpp#L63-L74)

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
- 性能模型集成
  - [PT Cache 响应线程:26-26](file://iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc#L26-L26)