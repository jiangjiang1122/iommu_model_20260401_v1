---
kind: logging_system
name: 日志与统计系统：StatsCollector 统一输出与 printf 调试并存
category: logging_system
scope:
    - '**'
source_files:
    - iommu/cache_src/common/stats_collector.h
    - iommu/cache_src/common/stats_collector.cpp
    - iommu/cache_config/default_config.json
    - iommu/cache_config/input_params_example.json
    - iommu/include/iommu_task.hh
    - main.cpp
---

本项目的日志/统计输出采用“框架式统计收集器 + 散点 printf 调试”的双轨模式，没有引入第三方日志库（如 spdlog、glog），而是自研 `StatsCollector` 作为结构化统计与文件输出的核心。