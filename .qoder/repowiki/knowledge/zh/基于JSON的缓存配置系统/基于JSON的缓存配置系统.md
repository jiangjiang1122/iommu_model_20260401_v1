---
kind: configuration_system
name: 基于JSON的缓存配置系统
category: configuration_system
scope:
    - '**'
source_files:
    - iommu/cache_config/default_config.json
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/types.h
    - iommu/cache_src/common/mini_json.h
---

该RISC-V IOMMU SystemC性能模型采用轻量级JSON配置文件驱动的配置系统，专门用于控制多Cache子系统（DC/PC/PT/Walker/MSIPT/Dedup）及失效机制的参数。

## 配置架构

核心组件包括json_config.h/.cpp提供load_config()和parse_config()两个接口，支持从文件路径或字符串加载配置；mini_json.h是自实现的极简JSON解析器，无外部依赖，支持对象、数组、基本类型和注释；types.h定义GlobalConfig、CacheConfig、InvalidationConfig、StatsConfig等配置结构体。

配置层次结构分为：全局配置(global)包含时钟周期(clock_period_ns)、随机种子(random_seed)；通用时序参数(cache_timing)包含仲裁器延迟、哈希延迟、读取集延迟、比较延迟等；各Cache独立配置(dc_cache、pc_cache、msipt_cache、pt_cache、dedup_cache、walker_cache)；失效配置(invalidation)包含延迟失效开关、LIB大小、VN位宽等；统计配置(statistics)包含输出文件、直方图、任务追踪等。

## 默认值与继承机制

配置系统采用分层默认值策略：common_cache_def为所有Cache提供基础默认值(64 sets, 4 ways, plru替换)；PT Cache默认使用srrip替换算法，1024 sets, 8 ways；Walker Cache分三级(C1/C2/C3)，分别对应不同way数和替换策略；Dedup Cache默认复用PT Cache几何参数，但num_rams=4, ram_fifo_depth=8。

## 配置加载流程

main.cpp到iommu_top.cc再到json_config.cpp最后到mini_json.h。虽然main.cpp中未直接调用配置加载函数，但Makefile将json_config.cpp纳入编译，表明配置系统在构建时可用。配置通过iommu_top::before_end_of_elaboration()在SystemC elaboration阶段初始化。

## 关键设计特点

可选字段支持：使用contains()检查字段存在性，未指定字段自动回退到默认值；向后兼容：支持unified_log_file作为output_file的别名；类型安全转换：所有JSON值通过显式类型转换，避免隐式转换错误；错误处理：文件打开失败抛出std::runtime_error，JSON解析错误有明确异常信息。

## 配置文件示例

default_config.json展示了完整的配置结构，包括时钟周期1ns、随机种子42、各Cache的sets/ways/replacement策略、多RAM并行配置(4 RAMs)、延迟失效(LIB=16, VN=4bit)等参数。