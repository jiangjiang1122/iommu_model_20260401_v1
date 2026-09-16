---
kind: error_handling
name: RISC-V IOMMU 错误处理机制：故障队列与中断上报
category: error_handling
scope:
    - '**'
source_files:
    - iommu/include/iommu_fault.hh
    - iommu/iommu_fun_model/iommu_faults.cc
    - iommu/include/iommu_registers.hh
    - iommu/iommu_fun_model/iommu_translate.cc
    - iommu/iommu_fun_model/iommu_atc.cc
    - iommu/iommu_fun_model/iommu_ats.cc
    - iommu/cache_src/common/mini_json.h
    - iommu/cache_src/cache/cache_base.h
---

该仓库实现了基于 RISC-V IOMMU 规范的错误处理机制，核心围绕**故障队列（Fault Queue）**、**页请求队列（Page Request Queue）**和**中断上报**构建。错误处理分为两个层面：硬件模拟层的结构化故障记录，以及缓存子系统层的 C++ 异常处理。

## 1. 故障记录与上报机制

### 故障记录结构
- `fault_rec_t`（`iommu_fault.hh:63-77`）：32 字节固定格式的故障记录，包含 CAUSE（12位）、PID（20位）、PV/PRIV、TTYP（事务类型）、DID、iotval、iotval2 等字段
- 支持多种故障原因常量：`RVI_IOMMU_ACCESS_FAULT`(0x01)、`RVI_IOMMU_GST_PAGE_FAULT`(0x21) 等

### 统一上报接口
- `report_fault()`（`iommu_faults.cc:9-159`）：唯一的外部故障上报入口，所有翻译路径通过此函数集中处理
- 实现 DTF（Disable Translation Fault）过滤逻辑，根据配置选择性屏蔽翻译相关故障
- 自动检查故障队列状态：启用标志（fqon/fqen）、溢出（fqof）、内存访问故障（fqmf）

### 寄存器映射
- `fqb_t`（`iommu_registers.hh:374-394`）：故障队列基址和大小配置
- `fqh_t`/`fqt_t`：软件头指针和硬件尾指针
- `fqcsr_t`（`iommu_registers.hh:550-564`）：故障队列控制状态寄存器

## 2. 故障传播路径

### 翻译路径中的故障点
- `iommu_translate.cc`：设备上下文查找失败、页表遍历错误时跳转到 `stop_and_report_fault` 标签
- `iommu_atc.cc`：权限检查失败（执行/读/写权限）跳转到 `page_fault` 标签
- `iommu_ats.cc`：ATS 请求处理中的各种错误场景调用 `report_fault()`

### 中断生成
- `generate_interrupt()` 在故障队列写入后触发 FAULT_QUEUE 中断
- 支持 MSI 中断和传统中断两种模式
- 中断挂起位（fip）在队列状态变化时设置

## 3. 缓存子系统的异常处理

### JSON 配置解析异常
- `mini_json.h`：自定义 JSON 解析器，使用 `std::runtime_error` 抛出类型不匹配、键不存在等错误
- 严格的类型转换操作符，失败时立即抛出异常而非返回错误码

### 资源管理异常安全
- `cache_base.h:826-829`：RAII 风格的异常安全包装，确保 RAM 端口释放不被异常中断
- 使用 `catch (...)` 捕获所有异常并重新抛出，保证资源清理

## 4. 设计约定与约束

### 故障优先级
1. 首先检查 DTF 位决定是否报告翻译相关故障
2. 验证故障队列是否启用且未处于错误状态
3. 检查队列溢出条件，设置 fqof 位并丢弃记录
4. 写入队列时检测内存访问故障，设置 fqmf 位

### 错误恢复策略
- 故障队列满或访问错误时，设置相应状态位并停止后续故障记录
- 需要软件显式清除错误位才能恢复故障记录功能
- 中断机制通知软件及时响应和处理

### 性能模型中的错误处理
- 性能统计模块使用 `cause` 字段分类统计不同类型的故障
- 虚拟化场景下区分普通页故障和 Guest 页故障（cause 20/21/23）
- 支持 A/D 位更新失败的隐式故障处理（GADE/SADE 位控制）