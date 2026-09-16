---
kind: dependency_management
name: C++/SystemC 项目依赖管理（Makefile + 系统库直连）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - compile_systemc.bat
---

本项目为基于 SystemC/TLM 的 RISC-V IOMMU 性能模型，采用纯 C++17 + Makefile 构建，**未使用任何包管理器或第三方依赖管理系统**。所有依赖通过编译期硬编码路径与链接器标志直接引入。

### 使用的系统与外部依赖
- **SystemC 2.x**：唯一的外部运行时库，通过 `-lsystemc` 链接，头文件位于 `/usr/include`，库文件位于 `/usr/lib/x86_64-linux-gnu`（Linux）或 `D:/TOOLS/systemc-2.3.3/install`（Windows，见 `compile_systemc.bat`）。
- **标准库**：`pthread`、`m`（数学库），由 `LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm` 声明。
- **编译器**：g++，要求 C++17 标准（`-std=c++17`）。

### 关键文件与配置位置
- `Makefile`：核心构建入口，集中定义 SYSTEMC_INCLUDE/SYSTEMC_LIB、CXXFLAGS、LIBS、源文件列表与测试场景。
- `build_cpp.sh` / `compile_and_test.sh`：辅助脚本，重复硬编码相同的 include 路径与链接参数。
- `compile_systemc.bat`：Windows 下从源码编译并安装 SystemC 的脚本，将结果安装到 `D:/TOOLS/systemc-2.3.3/install`。
- `build_log.txt` / `build_err.txt` / `build_out.txt`：历史构建日志，记录了不同平台下的实际编译命令。

### 架构与约定
- **无版本锁定**：没有 `systemc.lock`、`go.sum`、`package-lock.json` 等锁文件；SystemC 版本由安装位置决定，Makefile 中仅以变量形式引用路径。
- **无 vendoring**：所有 `.h` 头文件均通过 `-I` 路径包含，未将第三方库源码纳入仓库。
- **多平台适配**：通过 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 三个变量切换 Linux/Windows 路径，但默认指向系统安装目录而非仓库内路径。
- **调试宏驱动功能开关**：大量 `DEBUG_*` 宏（如 `DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`）在 Debug 模式下启用，Release 模式关闭，属于构建期依赖裁剪方式。

### 约束与观察到的规则
- 所有 C++ 源文件必须遵循 `-I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src ...` 的 include 路径约定，否则编译失败。
- 链接时必须保留 `-Wl,--allow-multiple-definition`，说明存在多处全局符号重定义（常见于 SystemC 仿真环境）。
- 测试场景通过 `TEST=xxx` 变量选择，每个场景对应一个独立的 `test_rp_*.cc` 线程文件，由 Makefile 中的条件分支注入对应的 `-DTEST_*` 编译宏。
- 未发现私有仓库、代理或镜像配置，依赖完全依赖本地已安装的 SystemC。