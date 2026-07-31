---
kind: dependency_management
name: C++/SystemC 项目依赖管理：Makefile + 系统库直连，无包管理器
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
---

本仓库是一个基于 SystemC/TLM-2.0 的 RISC-V IOMMU 仿真平台，采用纯 C++17 编写，**未使用任何第三方包管理器或依赖声明文件**。依赖管理完全通过 Makefile 和 shell 脚本手工维护，具体特征如下：

### 1. 使用的系统与外部依赖
- **SystemC (OSCI)**：作为唯一的外部框架依赖，通过 `-lsystemc` 链接，头文件位于 `/usr/include`，库位于 `/usr/lib/x86_6-linux-gnu`（见 `Makefile` 第 20–22、38 行）。
- **标准库与线程库**：链接 `-lpthread -lm`（第 38 行）。
- **编译器**：固定使用 `g++` / `gcc`，C++ 标准为 `c++17`（第 15–16、25 行）。
- **无 vendoring**：所有源码直接编译，无 `vendor/` 目录或子模块。

### 2. 关键文件与位置
- `Makefile`：构建系统的核心，集中定义源文件列表、包含路径、宏开关、测试场景与链接选项。
- `build_cpp.sh`、`compile_and_test.sh`：辅助构建脚本，硬编码了相同的编译参数与链接顺序。
- `link_and_run.sh`、`test_*.sh`、`check_*.sh`：运行与验证脚本，不引入额外依赖。
- `build/`：仅存放中间 `.o` 文件，为空目录，无锁定文件或缓存。

### 3. 架构与约定
- **扁平化源文件清单**：`Makefile` 中 `CXX_SOURCES` 变量显式列出每个 `.cc`/`.cpp` 文件（第 114–157 行），新增模块需手动加入该列表。
- **多场景切换**：通过 `TEST=` 变量选择不同测试线程源文件（如 `rand4k_singlestage`、`seq128k_twostage`、`sv48_bare` 等），由 `ifeq` 分支决定 `TEST_THREAD_SRC` 与 `TEST_FLAGS`。
- **调试宏开关**：通过 `DEBUG=1` 启用大量 `DDEBUG_*` 宏（第 30 行），用于控制各子系统输出。
- **SystemC 特定宏**：始终传入 `-DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK`（第 25 行）以兼容动态进程与版本检查。

### 4. 约束与规则
- **依赖必须预装在系统路径**：SystemC 必须已安装到 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`，否则构建失败（见 `Makefile` 第 20–22 行注释）。仓库未提供依赖安装脚本。
- **无版本锁定**：不存在 `go.mod`、`package.json`、`Cargo.toml`、`requirements.txt`、`CMakeLists.txt` 等任何依赖声明/锁定文件，依赖版本完全取决于宿主系统安装的 SystemC 版本。
- **跨平台限制**：路径硬编码为 Linux x86_64（`/usr/lib/x86_64-linux-gnu`），Windows 环境通过 `compile_systemc.bat` 单独处理（仓库根存在该文件但未在 Makefile 中集成）。
- **构建产物可重复性弱**：由于依赖系统级安装且无锁文件，同一源码在不同机器上可能因 SystemC 版本差异产生行为不一致。

### 总结
该项目采用最简化的手工依赖管理方式：所有第三方依赖（仅 SystemC）通过系统包管理器安装到标准路径，构建系统通过 Makefile 硬编码包含路径与链接标志。这种方式简单直接，但缺乏依赖版本锁定、可移植性与自动化更新能力，适合内部仿真模型快速迭代，不适合分发或跨团队协作。