---
kind: dependency_management
name: C++/SystemC 项目依赖管理：手工 Makefile + 系统库直连
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - compile_systemc.bat
---

本仓库是一个基于 SystemC 的 RISC-V IOMMU 模型，依赖管理采用最简化的 C++ 手工构建方式，未使用任何包管理器（如 vcpkg、Conan、pkg-config）或第三方依赖锁定机制。

**1. 使用的系统与工具**
- 编译器：g++ / gcc（C++17 标准）
- 构建系统：GNU Make（Makefile）+ 若干 Bash 辅助脚本（build_cpp.sh、compile_and_test.sh、link_and_run.sh 等）
- 外部依赖：SystemC 2.x（头文件位于 /usr/include，库位于 /usr/lib/x86_64-linux-gnu；Windows 下路径为 D:/TOOLS/systemc-2.3.3/install），以及 pthread、m 标准库
- 无 vendoring、无 lockfile、无私有仓库配置

**2. 关键文件与位置**
- `Makefile`：核心构建规则，定义 SYSTEMC_PREFIX/INCLUDE/LIB 路径、CXXFLAGS、所有源文件清单、目标可执行文件 iommu_model 及测试目标
- `build_cpp.sh`、`compile_and_test.sh`：手动编译 cache_src 子模块并链接的便捷脚本
- `compile_systemc.bat`：Windows 环境下 SystemC 源码编译脚本（SYSTEMC_SRC、SYSTEMC_INSTALL 变量）
- 各测试脚本（test_*.sh、check_*.sh）直接调用 g++ 并硬编码 -I/usr/include -L/usr/lib/x86_64-linux-gnu

**3. 架构与约定**
- 依赖声明方式：通过 `#include <systemc.h>` 和 `-lsystemc` 链接器标志显式引用 SystemC 库，版本由安装路径决定（常见为 2.3.3）
- 头文件搜索路径通过 Makefile 中大量 `-I` 参数集中管理，覆盖 iommu/、iommu/include/、cache_src/* 等目录
- 构建产物统一输出到 `build/` 目录，对象文件按源文件路径镜像组织
- 调试/发布模式通过 DEBUG=0/1 切换，影响 -O0/-O3 及一系列 DEBUG_* 宏

**4. 约定与约束**
- SystemC 必须预先安装在系统路径 `/usr`（Linux）或 `D:/TOOLS/systemc-2.3.3`（Windows），否则构建失败
- 所有源文件必须在 Makefile 的 CXX_SOURCES 列表中显式声明，新增文件需手动维护该列表
- 不支持跨平台自动探测依赖，不同环境需修改 Makefile 中的 SYSTEMC_INCLUDE/SYSTEMC_LIB 路径
- 无依赖版本锁定，升级 SystemC 可能导致 ABI 不兼容（需重新编译）
- 单元测试通过独立 g++ 命令编译 test_dedup_prefetch_unit.cpp，集成测试通过 shell 脚本驱动 iommu_model 运行