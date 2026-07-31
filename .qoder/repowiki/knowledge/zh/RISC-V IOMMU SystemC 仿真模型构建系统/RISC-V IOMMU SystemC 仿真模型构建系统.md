---
kind: build_system
name: RISC-V IOMMU SystemC 仿真模型构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
    - build_wsl.sh
    - run_wsl.sh
    - compile_systemc.bat
---

## 构建系统与工具链

该项目使用基于 **GNU Make** 的 C++/SystemC 构建系统，配合多个 Shell 脚本支持跨平台（Windows WSL/Linux）编译与测试。

### 核心构建工具
- **编译器**: g++ (C++17) / gcc，通过 `CXX` 和 `CC` 变量配置
- **构建系统**: GNU Make (`Makefile`) 作为主要入口
- **依赖库**: SystemC 2.x（通过 `-lsystemc` 链接），pthread、math 库
- **可选工具**: CMake（用于独立编译 SystemC 源码）

### 构建流程架构

**主构建入口** (`Makefile`)：
- 默认目标 `all` 生成 `iommu_model` 可执行文件
- 对象文件统一输出到 `build/` 目录，保持源码目录整洁
- 支持 `DEBUG=1/0` 切换调试/发布模式（Debug: -g -O0 + 大量 DEBUG_* 宏；Release: -O3 -DNDEBUG）
- 通过 `TEST` 变量选择不同测试场景，自动注入对应的源文件和编译宏

**多场景测试矩阵**：
- `rand4k_singlestage`（默认）：4KB随机读 + 单级地址翻译
- `seq128k_singlestage`：128KB顺序读 + 单级翻译
- `sv48_bare`：Sv48 + Bare 基础测试
- `seq128k_twostage` / `rand4k_twostage`：两阶段地址翻译
- 每个场景对应 `rp/test_rp_*.cc` 中的专用线程文件

### 辅助构建脚本

**Linux/WSL 环境**：
- `build_cpp.sh`：直接编译 cache_src 模块并链接，用于增量开发
- `compile_and_test.sh`：编译关键文件并运行，快速验证修改
- `link_and_run.sh`：仅链接已编译的对象并运行
- `build_wsl.sh`：在 WSL 本地文件系统编译后拷回 Windows 目录，解决跨文件系统性能问题
- `run_wsl.sh`：在 WSL 环境中运行仿真

**Windows 环境**：
- `compile_systemc.bat`：使用 CMake 编译安装 SystemC 源码到指定路径

### 依赖管理与路径配置

**SystemC 路径配置**：
- Linux: `/usr/include`, `/usr/lib/x86_64-linux-gnu`
- Windows: 通过环境变量 `SYSTEMC_HOME`, `SYSTEMC_INCLUDE`, `SYSTEMC_LIBDIR`
- 所有脚本硬编码路径，需根据实际安装位置调整

**包含路径**：
- 项目根目录、`./iommu`、`./iommu/include`、`./iommu/iommu_fun_model`、`./iommu/iommu_perf_model`、`./iommu/cache_src` 及其子目录
- 通过 `-I` 参数显式声明，无统一的 include 管理

### 构建约束与约定

**强制要求**：
- 必须启用 `-DSC_INCLUDE_DYNAMIC_PROCESSES` 和 `-DSC_DISABLE_API_VERSION_CHECK` 宏以兼容 SystemC 动态进程
- 链接时必须添加 `-Wl,--allow-multiple-definition` 以处理 SystemC 的多重定义
- 所有 .cc/.cpp 文件必须遵循相同的头文件包含顺序

**代码组织约定**：
- 构建产物统一输出到 `build/` 目录
- 测试场景通过 `TEST_THREAD_SRC` 变量选择，避免修改核心源文件
- 调试宏通过 `TEST_FLAGS` 传递，支持按场景定制编译选项

### 版本与发布策略

当前仓库未使用版本控制系统进行构建标记，版本号体现在目录名中（`iommu_model_20260401_v1`）。构建产物为单一可执行文件 `iommu_model`，无包管理器或分发机制。