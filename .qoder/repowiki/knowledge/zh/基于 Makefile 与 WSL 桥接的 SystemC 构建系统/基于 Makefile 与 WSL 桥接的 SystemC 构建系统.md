---
kind: build_system
name: 基于 Makefile 与 WSL 桥接的 SystemC 构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - build_wsl.sh
    - compile_systemc.bat
    - build_cpp.sh
    - run_wsl.sh
    - main.cpp
---

## 1. 构建系统概述
该项目采用 **GNU Make** 作为核心构建工具，配合 **g++ (C++17)** 编译器对 RISC-V IOMMU SystemC 性能模型进行编译、链接。由于 SystemC 库在 Windows 环境下配置复杂，项目设计了 **WSL (Windows Subsystem for Linux) 桥接构建流程**，通过在 WSL 本地文件系统临时拷贝源码并编译，再将可执行文件回拷至 Windows 目录，以解决跨文件系统编译性能低下的问题。

## 2. 核心构建逻辑
### 2.1 Makefile 架构
- **场景化编译 (`TEST` 变量)**：通过 `TEST` 变量（如 `rand4k_singlestage`, `seq128k_twostage`）动态选择测试线程源文件（位于 `rp/` 目录），并注入相应的预处理宏（如 `-DTEST_TWO_STAGE`）。
- **模块化源管理**：明确列出了功能模型 (`iommu_fun_model`)、性能模型 (`iommu_perf_model`)、缓存子系统 (`cache_src`) 以及总线/内存模拟模块 (`pcienoc`, `slink`, `ddr`) 的源文件。
- **调试支持**：通过 `DEBUG` 变量控制编译选项，开启时注入大量调试宏（如 `DEBUG_TRANSLATION`, `DEBUG_FAULTS`）并启用 `-g -O0`。
- **单元测试集成**：提供了 `test_dedup_unit` 等独立目标，用于快速验证 PT Cache 去重与预取逻辑。

### 2.2 编译环境配置
- **SystemC 依赖**：默认指向 Linux 系统路径 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`。
- **链接选项**：使用 `-Wl,--allow-multiple-definition` 处理 SystemC 常见的符号重复定义问题，并链接 `pthread` 以支持并发仿真。

## 3. 关键脚本与自动化
- **`build_wsl.sh`**：核心构建脚本。执行“拷贝 -> WSL 本地编译 -> 回拷可执行文件 -> 清理”的四步流程，显著提升了在 Windows 宿主环境下的开发迭代速度。
- **`compile_systemc.bat`**：Windows 批处理脚本，用于在 Windows 端通过 CMake 编译和安装 SystemC 库，为整个项目提供基础运行库支持。
- **`run_wsl.sh`**：简单的运行封装，确保在正确的目录下启动仿真。

## 4. 开发者规范与建议
1. **构建方式选择**：在 Windows 下开发时，强烈建议使用 `./build_wsl.sh [SCENARIO]` 进行编译，避免直接在挂载的 NTFS 分区上运行 `make` 导致的性能瓶颈。
2. **场景切换**：通过 `make TEST=seq128k_twostage` 等方式切换测试场景，无需手动修改源码或 Makefile。
3. **调试模式**：遇到仿真逻辑问题时，使用 `make DEBUG=1` 重新编译以获取详细的翻译、故障及中断日志。
4. **环境准备**：确保 WSL 环境中已正确安装 SystemC 库，或在 Windows 端运行 `compile_systemc.bat` 完成库的初始化。