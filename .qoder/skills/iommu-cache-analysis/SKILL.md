---
name: iommu-cache-analysis
description: 分析 IOMMU Cache 子系统的架构、访问类型、执行流程、延时计算和原子阶段约束。当用户询问 Cache 内部机制、性能瓶颈、串行约束或需要生成技术文档时使用。触发词包括：Cache分析、访问类型、执行流程、延时计算、原子阶段、串行执行、RAM互斥。
---

# IOMMU Cache 代码分析

## 分析维度

对任意 Cache 模块，从以下 6 个维度进行系统分析：

### 1. 访问类型识别
- 查找入口函数（如 `lookup_pt`, `fill_pt`, `invalidate_*`）
- 识别操作类型：LOOKUP / FILL / INVALIDATE
- 统计各类型的触发场景和调用路径

### 2. 内部执行流程
- 绘制流程图：从 FIFO 读取 → 执行函数 → 响应路由
- 识别分支逻辑（HIT/MISS、替换策略等）
- 标注关键函数调用链

### 3. 延时计算模型
- 查找延时配置（`default_config.json` 或头文件）
- 列出各操作的 cycle 计算公式
- 计算典型延时值（ns）

### 4. 原子阶段识别
- 查找互斥机制（`arbitrate_ram_access`, `ram_port_busy_`）
- 识别串行约束点
- 标注哪些操作必须独占资源

### 5. 调度策略
- 识别调度线程（如 `pt_scheduler_thread`）
- 分析调度算法（乒乓、优先级、轮询）
- 识别 FIFO 队列和流控机制

### 6. 性能统计维度
- 查找统计接口（`stats_.record_*`）
- 识别统计维度（按阶段、按类型、按区间）
- 列出关键性能指标

## 分析流程

```
Task Progress:
- [ ] Step 1: 定位目标 Cache 模块的源文件
- [ ] Step 2: 识别入口函数和访问类型
- [ ] Step 3: 追踪执行流程和分支逻辑
- [ ] Step 4: 提取延时配置和计算公式
- [ ] Step 5: 识别互斥机制和原子阶段
- [ ] Step 6: 分析调度策略和 FIFO 流控
- [ ] Step 7: 整理统计维度和性能指标
- [ ] Step 8: 生成结构化分析报告
```

## 输出格式

使用 [analysis-template.md](analysis-template.md) 模板生成报告，包含：

1. **访问类型汇总表**
2. **执行流程图**（ASCII 或 Mermaid）
3. **延时计算表**
4. **原子阶段约束说明**
5. **性能统计维度**

## 示例用法

**用户输入**：
```
分析 PT Cache 的访问类型和执行流程
```

**Agent 行为**：
1. 读取 `pt_cache.cpp` 和 `cache_subsystem.cpp`
2. 识别 `lookup_pt`, `fill_pt`, `invalidate_*` 等入口
3. 追踪 `arbitrate_ram_access` 互斥机制
4. 提取 `default_config.json` 中的延时参数
5. 生成结构化分析报告

## 参考资源

- Cache 模块文件：`iommu/cache_src/cache/*.cpp`
- 调度逻辑：`iommu/cache_src/subsystem/cache_subsystem.cpp`
- 延时配置：`iommu/cache_config/default_config.json`
- 基类模板：`iommu/cache_src/cache/cache_base.h`

## 支持的 Cache 模块

| 模块 | 源文件 | 说明 |
|-----|-------|-----|
| PT Cache | `pt_cache.cpp` | 页表缓存，1024组×8路 |
| DC Cache | `dc_cache.cpp` | 设备上下文缓存 |
| PC Cache | `pc_cache.cpp` | 进程上下文缓存 |
| Walker Cache | `walker_cache.cpp` | 页表遍历缓存（C1/C2/C3） |
| MSIPT Cache | `msipt_cache.cpp` | MSI页表缓存 |
| Dedup Cache | `dedup_cache.cpp` | 去重缓存（V3.0） |
