---
kind: dependency_management
name: 依赖管理 — SystemC C++ 模型构建与外部库依赖
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - main.cpp
---

本仓库是一个基于 SystemC 的 RISC-V IOMMU 性能模型，依赖管理采用**纯 Makefile + shell 脚本**的手动构建方式，未使用任何包管理器（如 vcpkg、Conan、pkg-config）或第三方依赖锁定机制。所有依赖通过编译期路径和链接参数显式声明。

### 1. 使用的系统与工具
- **编译器**: g++ (C++17) / gcc，通过 `Makefile` 中的 `CXX = g++`、`CC = gcc` 指定。
- **构建系统**: GNU Make，核心规则集中在根目录 `Makefile`。
- **SystemC 框架**: 作为唯一外部 C++ 框架依赖，通过 `-lsystemc -L$(SYSTEMC_LIB)` 链接。
- **辅助库**: pthread、math (`-lpthread -lm`)。
- **Windows 环境**: 提供 `compile_systemc.bat` 用于从源码编译并安装 SystemC 2.3.3。

### 2. 关键文件与位置
- `Makefile` — 主构建入口，定义所有源文件、包含路径、编译/链接选项及测试场景。
- `build_cpp.sh` — Linux 下手动编译 cache_src 子系统的辅助脚本。
- `compile_systemc.bat` — Windows 下 SystemC 源码编译与安装脚本。
- `build/` — 构建产物输出目录（当前为空，由 Makefile 生成）。
- `main.cpp` — 程序入口点。

### 3. 架构与约定
- **依赖声明方式**: 所有依赖在 `Makefile` 中以硬编码形式声明：
  - 包含路径: `SYSTEMC_INCLUDE = /usr/include`，并通过 `-I$(SYSTEMC_INCLUDE) -I./iommu ...` 逐一添加。
  - 库路径: `SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu`，链接时 `-L$(SYSTEMC_LIB) -lsystemc`。
  - 宏定义: 通过 `-DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK` 启用 SystemC 动态进程。
- **无版本锁定**: 没有 `go.mod`、`package.json`、`Cargo.toml` 等依赖清单；SystemC 版本由用户本地安装决定（Windows 脚本固定为 2.3.3）。
- **无 vendoring**: 未将 SystemC 头文件或库文件纳入仓库，需预先安装在系统路径。
- **多平台支持**: Linux 默认假设 SystemC 已安装至 `/usr`；Windows 通过批处理脚本从源码构建。

### 4. 约定与约束
- **SystemC 必须预装**: 构建前需在系统中安装 SystemC 2.x，头文件位于 `/usr/include`，库文件位于 `/usr/lib/x86_64-linux-gnu`（Linux）或自定义路径（Windows 通过 `compile_systemc.bat` 设置环境变量）。
- **C++17 标准强制**: 所有编译命令均使用 `-std=c++17`。
- **调试/发布模式**: 通过 `DEBUG=1`（默认）启用 `-g -O0` 及大量 `DEBUG_*` 宏；`DEBUG=0` 启用 `-O3 -DNDEBUG`。
- **测试场景通过宏切换**: 不同测试用例通过 `TEST=` 变量选择，对应不同的 `-DTEST_*` 编译宏，而非运行时配置。
- **链接器选项**: 使用 `-Wl,--no-as-needed -Wl,--allow-multiple-definition` 以兼容 SystemC 的动态链接行为。
- **无私有仓库/代理**: 未配置 GOPRIVATE、npm registry 或任何私有源，所有依赖均来自系统包管理器或源码编译。