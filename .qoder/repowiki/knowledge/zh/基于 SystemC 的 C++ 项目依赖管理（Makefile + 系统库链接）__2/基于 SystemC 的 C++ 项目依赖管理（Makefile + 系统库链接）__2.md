---
kind: dependency_management
name: 基于 SystemC 的 C++ 项目依赖管理（Makefile + 系统库链接）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - compile_wsl.sh
    - link_and_run.sh
    - main.cpp
---

## 1. 使用的系统与工具

本项目是一个纯 C++/SystemC 仿真模型，**没有使用任何语言级包管理器**（如 Conan、vcpkg、npm、pip、go mod 等），也没有 vendoring 第三方源码。所有外部依赖通过 **GNU Make + g++** 直接链接到宿主系统的已安装库：

- **SystemC 2.x**：唯一的第三方运行时库，通过 `-lsystemc -Wl,--no-as-needed` 链接。
- **POSIX 标准库**：`-lpthread -lm`。
- **编译器**：g++，要求 C++17（`-std=c++17`）。

构建入口是根目录的 `Makefile`，另有辅助脚本 `build_cpp.sh`、`compile_wsl.sh`、`compile_systemc.bat` 用于不同环境。

## 2. 关键文件

- `Makefile`：定义全部编译/链接参数、源文件清单、测试场景选择、目标产物 `iommu_model`。
- `build_cpp.sh`：逐文件编译 `iommu/cache_src/*.cpp` 并链接的 Bash 脚本，作为 Make 之外的备选路径。
- `compile_systemc.bat`：在 Windows 上从源码编译并安装 SystemC 2.3.3 的批处理脚本，设置 `SYSTEMC_HOME` / `SYSTEMC_INCLUDE` / `SYSTEMC_LIBDIR` 环境变量。
- `compile_wsl.sh`：在 WSL 环境中执行 `make clean && make all` 的封装脚本。
- `link_and_run.sh`、`run_wsl.sh`、`test_*.sh`、`check_*.sh`：运行与验证脚本，不引入新依赖。

## 3. 架构与约定

### 3.1 依赖声明方式

依赖不是以声明式清单（如 `package.json`、`go.mod`、`CMakeLists.txt`）集中描述，而是**硬编码在构建脚本中**：

| 位置 | 内容 |
|---|---|
| `Makefile` L21–23 | `SYSTEMC_PREFIX = /usr`、`SYSTEMC_INCLUDE = /usr/include`、`SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu` |
| `Makefile` L26 | `CXXFLAGS` 通过 `-I$(SYSTEMC_INCLUDE)` 将 SystemC 头文件纳入搜索路径 |
| `Makefile` L39 | `LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm` |
| `Makefile` L311 | 链接阶段 `-L$(SYSTEMC_LIB) $(LIBS)` |
| `build_cpp.sh` L4/L20 | 同样的 `-I/usr/include` 和 `-L/usr/lib/x86_64-linux-gnu -lsystemc` |
| `compile_systemc.bat` L5–7 | Windows 下 SystemC 源码路径 `D:\TOOLS\systemc-2.3.3`，安装到 `D:\TOOLS\systemc-2.3.3\install` |

### 3.2 版本管理策略

- **SystemC 版本**：Windows 脚本固定为 `systemc-2.3.3`；Linux 默认假设系统包管理器安装的任意 2.x 版本位于 `/usr`。**仓库内没有锁定具体版本号**。升级 SystemC 需要手动修改 `Makefile` 中的 `SYSTEMC_*` 路径或重新安装系统包。
- **C++ 标准**：固定为 C++17（`-std=c++17`），由编译器保证。
- **无 lockfile**：不存在 `package-lock.json`、`go.sum`、`Pipfile.lock`、`conan.lock` 等任何锁文件。

### 3.3 本地源码依赖组织

项目自身代码按功能模块分目录组织，并通过 `#include` 相对路径引用，而非通过包管理器分发：

- `iommu/iommu_fun_model/`、`iommu/iommu_perf_model/`、`iommu/include/`：IOMMU 核心逻辑。
- `iommu/cache_src/{cache,common,replacement,subsystem}`：缓存子系统源码。
- `rp/`、`pcienoc/`、`slink/`、`ddr/`：Root Port、PCIe NoC、串行链路、DDR 测试驱动。
- `main.cpp`：SystemC 仿真入口。

这些模块之间通过 `Makefile` 中的 `CXX_SOURCES` 列表显式参与编译，而不是通过自动依赖发现。

### 3.4 多平台构建约定

| 平台 | 构建方式 | 依赖安装位置 |
|---|---|---|
| Linux (原生/WSL) | `make all` 或 `./compile_wsl.sh` | 系统包管理器安装的 SystemC，位于 `/usr` |
| Windows | `compile_systemc.bat` 先编译安装 SystemC，再手动用 MSVC/g++ 编译 | `D:\TOOLS\systemc-2.3.3\install` |

## 4. 约定与约束

- **必须预先安装 SystemC 头文件和库**：`Makefile` 假定 `systemc.h` 可在 `-I/usr/include` 找到，`libsystemc.so` 可在 `-L/usr/lib/x86_64-linux-gnu` 链接。若不在该路径，需通过 `SYSTEMC_INCLUDE` / `SYSTEMC_LIB` 覆盖（见 `build_log.txt` 中 `SYSTEMC_INCLUDE=D:/TOOLS/syst...` 的调用示例）。
- **SystemC API 版本检查被禁用**：编译时强制传入 `-DSC_DISABLE_API_VERSION_CHECK`，使模型可兼容不同版本的 SystemC 二进制接口。
- **动态进程支持**：通过 `-DSC_INCLUDE_DYNAMIC_PROCESSES` 启用 SystemC 动态进程。
- **调试/发布开关**：`DEBUG=1`（默认）开启 `-g -O0 -DDEBUG` 及大量 `DEBUG_*` 宏；`DEBUG=0` 切换为 `-O3 -DNDEBUG`。
- **测试场景通过 `TEST=` 变量选择**：`make TEST=rand4k_singlestage`、`make TEST=seq128k_twostage`、`make TEST=msi_perf` 等，每个场景对应 `rp/test_rp_*.cc` 中的一个线程源文件，并通过 `TEST_FLAGS` 注入编译期配置宏（如 `TEST_CFG_PTW_MAX_OUTSTANDING_TASKS`、`TEST_TWO_STAGE`）。
- **无私有注册表**：未配置任何私有 npm/pip/Conan/Cargo 注册表，所有依赖来自系统路径或源码树。
- **无 CI 自动化**：仓库中没有 GitHub Actions / Jenkins / GitLab CI 等流水线，依赖安装与构建完全依赖开发者本地环境。

## 5. 总结

本项目的“依赖管理”本质上是**手工维护的 Makefile + 系统级库链接**模式：SystemC 作为唯一外部依赖，其路径、版本、头文件/库目录全部硬编码在 `Makefile` 和若干脚本中，没有包管理器、lockfile 或 vendoring 机制。跨平台差异通过 `compile_systemc.bat`（Windows）和 `compile_wsl.sh`（WSL/Linux）两个入口脚本分别处理。这种方式的优点是简单直接，缺点是缺乏可复现的版本锁定和便捷的依赖迁移能力。