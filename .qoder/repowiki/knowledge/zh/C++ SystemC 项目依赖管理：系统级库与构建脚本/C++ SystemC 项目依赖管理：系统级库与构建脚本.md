---
kind: dependency_management
name: C++ SystemC 项目依赖管理：系统级库与构建脚本
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
---

本项目为基于 SystemC 的 RISC-V IOMMU 性能模型，采用 C++17 + Makefile 构建方式，依赖管理策略如下：

**外部依赖来源**
- **SystemC 2.3.3**：通过系统包管理器安装（Linux 路径 `/usr/include`、`/usr/lib/x86_64-linux-gnu`）或 Windows 下使用 `compile_systemc.bat` 从源码编译安装到 `D:\TOOLS\systemc-2.3.3\install`。Windows 脚本通过 CMake 配置、编译并安装 SystemC，同时设置 `SYSTEMC_HOME`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIBDIR` 环境变量。
- **标准库依赖**：`-lpthread`（线程）、`-lm`（数学库），在 `LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm` 中声明。
- **无第三方 C/C++ 包管理器**：项目中不存在 `go.mod`、`package.json`、`CMakeLists.txt`、`vcpkg.json`、`Conanfile` 等依赖声明文件，也未使用 vendoring 策略。

**依赖版本管理**
- SystemC 版本固定为 2.3.3（见 `compile_systemc.bat` 中的路径），通过硬编码路径和编译脚本锁定。
- C++ 标准固定为 C++17（`-std=c++17`），编译器为 g++/gcc。
- 未使用锁文件或版本约束文件，依赖版本由构建脚本和环境变量决定。

**构建与链接约定**
- 所有 `.cc`/`.cpp` 源文件通过 `Makefile` 中的 `CXX_SOURCES` 列表显式声明，对象文件输出到 `build/` 目录。
- 链接时使用 `-Wl,--allow-multiple-definition` 允许重复符号定义，这是 SystemC 动态进程模型的常见需求。
- 头文件包含路径通过 `-I` 参数集中管理，包括 `./iommu`、`./iommu/include`、`./iommu/cache_src` 等子目录。
- 调试/发布模式通过 `DEBUG=1/0` 控制，影响优化级别和调试宏定义。

**测试与验证依赖**
- Python 脚本用于数据分析（`tmp/` 目录下多个 `analyze_*.py`、`calc_*.py`、`plot_*.py` 脚本），但未在构建系统中声明依赖。
- Shell 脚本用于自动化测试和构建流程（`test_*.sh`、`check_*.sh`、`run_*.sh` 等）。

**约束与限制**
- 依赖完全依赖本地环境配置，未提供跨平台自动下载机制。
- 无依赖更新自动化流程，需手动更新 `Makefile`、`build_cpp.sh`、`compile_systemc.bat` 中的路径和版本。
- 未使用静态链接或打包策略，运行时依赖 SystemC 动态库。