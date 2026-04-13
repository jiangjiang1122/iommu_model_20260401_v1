# RISC-V IOMMU SystemC Model

## 项目概述

这是一个基于SystemC的RISC-V IOMMU（Input-Output Memory Management Unit）模型，用于模拟RISC-V架构下的IOMMU功能，包括地址转换、故障处理、设备上下文管理等。

## 环境要求

**重要：根据项目要求，所有编译和运行操作必须在WSL（Windows Subsystem for Linux）环境中进行。**

- WSL2（推荐）
- Ubuntu或其他兼容的Linux发行版
- SystemC库（安装在 `/usr` 目录下）
- GCC/G++ 编译器
- GNU Make

## 编译方法

### 使用编译脚本（推荐）

项目根目录下提供了一个编译脚本，可以自动完成清理和编译过程：

```bash
# 确保脚本具有执行权限
chmod +x compile_wsl.sh

# 运行编译
./compile_wsl.sh
```

### 手动编译

进入WSL终端并导航至项目目录：

```bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
make clean
make all
```

或者使用调试模式编译（默认）：

```bash
make clean
make DEBUG=1
```

使用发布模式编译（优化，无调试信息）：

```bash
make clean
make DEBUG=0
```

## 运行模型

编译成功后，会在项目根目录生成 `iommu_model` 可执行文件：

```bash
./iommu_model
```

## 项目结构

- `iommu/` - IOMMU核心实现模块
  - `iommu_top.cc/hh` - 顶层模块
  - `iommu_translate.cc/hh` - 地址转换引擎
  - `iommu_command_queue.cc/hh` - 命令队列处理器
  - `iommu_atc.cc/hh` - 地址转换缓存
  - 其他IOMMU功能模块
- `rp/` - Root Port测试模块
- `pcienoc/` - PCIe NoC接口模块
- `ddr/` - DDR仿真模块
- `main.cpp` - 主程序入口
- `Makefile` - 构建配置文件

## 调试

如需调试，请参考 `GDB_DEBUG_GUIDE.md` 文档，其中包含了详细的GDB调试指南。

## 注意事项

1. **必须在WSL环境中编译和运行**：这是项目的关键要求
2. SystemC库必须正确安装在Linux环境中
3. 默认启用调试模式，包含大量调试输出
4. 编译生成的可执行文件可以直接在WSL中运行

## 开发工具

- SystemC 2.3.x
- GCC/G++ 支持 C++11
- GDB 调试器