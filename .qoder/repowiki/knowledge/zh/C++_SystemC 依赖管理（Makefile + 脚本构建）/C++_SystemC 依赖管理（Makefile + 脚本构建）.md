---
kind: dependency_management
name: C++/SystemC 依赖管理（Makefile + 脚本构建）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
---

本仓库是一个基于 SystemC 的 C++ 项目，依赖管理采用传统的 Makefile + Shell 脚本方式，未使用任何现代包管理器（如 vcpkg、Conan、pkg-config 等），所有第三方库均通过系统路径直接链接。

**使用的系统与第三方依赖**
- **SystemC**：作为核心仿真框架，通过 `-lsystemc` 链接，头文件位于 `/usr/include`，库文件位于 `/usr/lib/x86_64-linux-gnu`，由 `SYSTEMC_PREFIX=/usr`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量集中配置。
- **标准库与线程库**：链接 `-lpthread -lm`，编译启用 C++17（`-std=c++17`）。
- **无 vendoring / 无锁文件**：仓库中不存在 `go.mod`、`package.json`、`CMakeLists.txt`、`vcpkg.json`、`conanfile.txt` 等依赖声明或锁定文件，也未见 vendor/ 目录。

**构建与依赖解析方式**
- 主构建入口为根目录 `Makefile`，通过 `g++` 直接编译所有 `.cc/.cpp` 源文件并链接到 `iommu_model` 可执行文件。
- 多个辅助脚本（`build_cpp.sh`、`compile_and_test.sh`、`link_and_run.sh`、`test_*.sh`）重复硬编码了相同的 `-I` 包含路径和 `-L` 库路径，形成多入口但一致的依赖声明。
- 编译器标志通过 `CXXFLAGS` 变量统一传递，包含大量 `-DDEBUG_*` 宏定义用于条件编译调试代码。

**约定与约束**
- SystemC 必须预先安装到系统路径 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`，否则构建失败；不同环境需修改 `Makefile` 中的 `SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB`。
- 所有源文件通过显式列举在 `CXX_SOURCES` 变量中参与构建，新增文件需手动添加到该列表。
- 链接时强制使用 `-Wl,--allow-multiple-definition` 以容忍 SystemC 动态进程的多重定义问题。
- 测试场景通过 `TEST=` 变量选择不同线程源文件，配合对应的 `-DTEST_*` 宏进行编译期配置。

**风险与不足**
- 缺乏依赖版本锁定，跨机器/跨 CI 环境可能因 SystemC 版本差异导致行为不一致。
- 构建脚本分散且重复，维护成本较高，建议统一收敛到单一构建系统（如 CMake）。