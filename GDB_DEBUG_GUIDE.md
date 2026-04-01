# RISC-V IOMMU 模型 GDB 调试指南

## 概述
本文档提供了如何使用 GDB 调试 RISC-V IOMMU 模型的详细说明。

## 准备工作

### 1. 编译调试版本
要启用 GDB 调试功能，需要使用以下命令编译项目：

```bash
make clean
make DEBUG=1
```

这将在编译时启用调试符号（-g）、禁用优化（-O0）并启用各种调试宏定义。

### 2. 检查编译产物
编译完成后，应该有一个名为 `iommu_model` 的可执行文件，其中包含调试符号。

## 使用 GDB 调试

### 方法一：使用提供的调试脚本
运行提供的调试脚本：
```bash
chmod +x gdb_debug.sh
./gdb_debug.sh
```

### 方法二：直接使用 GDB
手动启动 GDB：
```bash
gdb ./iommu_model
```

## 常用 GDB 命令

### 基本命令
- `run` 或 `r` - 运行程序
- `continue` 或 `c` - 继续执行程序
- `quit` 或 `q` - 退出 GDB
- `help` 或 `h` - 获取帮助

### 断点管理
- `break <function_name>` 或 `b <function_name>` - 在函数入口设置断点
- `break <filename:line_number>` - 在指定文件的行号设置断点
- `info breakpoints` 或 `i b` - 显示所有断点
- `delete <breakpoint_number>` 或 `d <breakpoint_number>` - 删除指定断点
- `delete` - 删除所有断点

### 单步调试
- `step` 或 `s` - 单步执行（进入函数）
- `next` 或 `n` - 单步执行（跳过函数）
- `finish` - 完成当前函数并返回
- `until <line_number>` - 运行直到指定行

### 变量和内存检查
- `print <variable_name>` 或 `p <variable_name>` - 打印变量值
- `print <variable_name>@<count>` - 打印数组元素
- `display <expression>` - 每次停止时自动显示表达式值
- `info locals` - 显示所有局部变量
- `x/<format> <address>` - 检查内存内容

### 调用栈
- `backtrace` 或 `bt` - 显示调用栈
- `frame <number>` - 切换到指定栈帧
- `info frame` - 显示当前栈帧信息

## 针对 IOMMU 模型的调试技巧

### 1. 关键断点设置
以下是针对 IOMMU 模型的一些关键断点建议：

```gdb
# 在主翻译函数处设置断点
(gdb) break iommu_translate_iova

# 在设备上下文定位函数处设置断点
(gdb) break locate_device_context

# 在地址翻译缓存相关函数处设置断点
(gdb) break lookup_ioatc_iotlb
(gdb) break cache_ioatc_iotlb

# 在命令队列处理函数处设置断点
(gdb) break CQ_Monitor_Process_Thread
```

### 2. 调试地址翻译问题
如果遇到地址翻译错误，可以在以下位置设置断点：

```gdb
# 在地址翻译入口处
(gdb) break iommu_translate_iova

# 在两阶段地址翻译函数处
(gdb) break two_stage_address_translation
(gdb) break second_stage_address_translation

# 在故障报告函数处
(gdb) break report_fault
```

### 3. 查看关键数据结构
在调试过程中，可以查看以下关键数据结构：

```gdb
# 查看 IOMMU 实例
(gdb) print iommu->reg_file

# 查看 DDTP 寄存器
(gdb) print iommu->reg_file.ddtp

# 查看功能控制寄存器
(gdb) print iommu->reg_file.fctl

# 查看能力寄存器
(gdb) print iommu->reg_file.capabilities
```

## 调试示例

### 示例 1：调试地址翻译失败
```bash
# 启动 GDB
gdb ./iommu_model

# 设置断点
(gdb) break iommu_translate_iova
(gdb) break locate_device_context
(gdb) break report_fault

# 运行程序
(gdb) run

# 当程序在断点处停止时，检查变量值
(gdb) print req->device_id
(gdb) print req->tr.iova
(gdb) print iommu->reg_file.ddtp.raw
```

### 示例 2：单步调试特定功能
```bash
# 启动 GDB 并立即运行
gdb -ex run ./iommu_model

# 当程序运行时按 Ctrl+C 停止
# 然后设置断点并继续
(gdb) break iommu_translate_iova
(gdb) continue

# 在断点处单步执行
(gdb) step
(gdb) print iommu->reg_file.ddtp.iommu_mode
(gdb) next
```

## 调试常见问题

### 1. 符号不可用
如果 GDB 报告找不到符号，请确保：
- 使用 `make DEBUG=1` 编译
- 没有 strip 掉调试符号
- 源代码路径正确

### 2. 断点无法命中
- 确认函数名拼写正确
- 检查是否在正确的编译模式下（DEBUG=1）
- 确认代码路径确实被执行

### 3. 性能考虑
由于启用了调试模式（-O0），程序运行速度会显著降低，这是正常的。

## 高级调试技术

### 条件断点
```gdb
# 只在满足条件时中断
(gdb) break iommu_translate_iova if req->device_id == 0x2345
```

### 命令断点
```gdb
# 在断点命中时自动执行命令
(gdb) break iommu_translate_iova
(gdb) commands
Type commands for breakpoint(s) 1, one per line.
End with a line saying just "end".
>print req->tr.iova
>print req->device_id
>continue
>end
```

### 跟踪点
```gdb
# 不中断程序执行，仅记录信息
(gdb) trace iommu_translate_iova
(gdb) actions
>printf "IOVA: 0x%lx, Device ID: 0x%x\n", req->tr.iova, req->device_id
>end
(gdb) run
```

## 结论
通过使用 GDB 和上述调试技术，您可以有效地调试 RISC-V IOMMU 模型，识别和解决地址翻译及其他功能方面的问题。