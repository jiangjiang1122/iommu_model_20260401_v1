---
kind: dependency_management
name: C++/SystemC 手工构建与外部库依赖管理
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - compile_and_test.sh
    - link_and_run.sh
---

本仓库为基于 SystemC 的 RISC-V IOMMU 性能模型，采用纯 C++17 + Makefile 手工构建方式，未使用任何包管理器（如 vcpkg、Conan、vcpkg、pkg-config 等），所有第三方依赖均为系统级安装的外部库，通过编译器参数显式链接。

**依赖系统与工具链**
- 语言标准：c++17（`-std=c++17`）
- 编译器：g++ / gcc（Linux 环境）；Windows 下通过 `compile_systemc.bat` 用 CMake 编译安装 SystemC 2.3.3
- 构建系统：GNU Make（`Makefile`），辅以多个 shell 脚本（`build_cpp.sh`、`compile_and_test.sh`、`link_and_run.sh`、`test_*.sh`）重复声明编译/链接命令
- 调试器：GDB（`gdb_debug.sh`）

**外部依赖清单**
- **SystemC 2.x**：唯一核心第三方库，通过 `-lsystemc -L$(SYSTEMC_LIB)` 链接。Linux 默认路径 `/usr/lib/x86_64-linux-gnu`，Windows 路径由 `compile_systemc.bat` 设置为 `D:\TOOLS\systemc-2.3.3\install\lib`。头文件路径由 `SYSTEMC_INCLUDE` 控制（`/usr/include` 或 `D:/TOOLS/systemc-2.3.3/install/include`）。
- **系统库**：`-lpthread`（POSIX 线程）、`-lm`（数学库）
- 无其他第三方 C/C++ 库（如 JSON 解析器等均自实现于 `iommu/cache_src/common/mini_json.h`、`json_config.cpp`）

**版本锁定与更新策略**
- 无 lockfile 或版本清单文件（无 `go.mod`、`package.json`、`Cargo.lock`、`requirements.txt` 等）
- SystemC 版本在 `compile_systemc.bat` 中硬编码为 2.3.3，Linux 构建依赖系统已安装的 SystemC 包
- 依赖版本更新需手动修改 `Makefile` 中的 `SYSTEMC_PREFIX/INCLUDE/LIB` 路径及 `compile_systemc.bat` 中的源码路径

**构建约定与约束**
- 所有源文件通过 `Makefile` 中 `CXX_SOURCES` 变量显式列举，新增文件需同步更新该列表
- 对象文件统一输出到 `build/` 目录，按源文件路径镜像组织（`build/%.o: %.cc` 规则）
- 链接时强制使用 `-Wl,--allow-multiple-definition` 以容忍 SystemC 多定义问题
- Windows 环境下必须先运行 `compile_systemc.bat` 完成 SystemC 编译安装并设置 `SYSTEMC_HOME`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIBDIR` 环境变量
- Linux 环境下要求 SystemC 已通过包管理器安装至 `/usr` 前缀

**私有仓库与分发**
- 无私有包仓库配置（无 `.npmrc`、`.pypirc`、`GOPRIVATE`、`~/.conan` 等）
- 项目本身作为单一可执行目标 `iommu_model` 分发，不发布为库