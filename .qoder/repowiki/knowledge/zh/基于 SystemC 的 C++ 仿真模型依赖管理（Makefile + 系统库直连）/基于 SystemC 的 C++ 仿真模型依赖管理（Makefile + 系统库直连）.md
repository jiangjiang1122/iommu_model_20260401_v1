---
kind: dependency_management
name: 基于 SystemC 的 C++ 仿真模型依赖管理（Makefile + 系统库直连）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - .gitignore
---

## 1. 使用的系统与工具

本项目是一个基于 **SystemC/TLM 2.0** 的 RISC-V IOMMU 性能与功能仿真模型，语言为 C++17。仓库中 **没有使用任何包管理器**（无 `go.mod`、`package.json`、`CMakeLists.txt`、`vcpkg.json`、`conanfile.py`、`Cargo.toml` 等），所有第三方依赖通过 **系统级安装 + Makefile 直接链接** 的方式管理。

- 编译器：`g++` / `gcc`（C++17，`-std=c++17`）
- 构建系统：GNU `make`（根目录 `Makefile`）
- 外部依赖：SystemC 运行时库（`libsystemc`）、POSIX 线程（`pthread`）、数学库（`m`）
- Windows 环境：提供 `compile_systemc.bat`，用 CMake 从源码编译并安装 SystemC 到本地路径后，再设置 `SYSTEMC_HOME` / `SYSTEMC_INCLUDE` / `SYSTEMC_LIBDIR` 环境变量供后续编译使用

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 唯一构建入口，声明 SystemC 头文件/库路径、编译选项、源文件清单、测试场景选择、链接参数 |
| `build_cpp.sh` | Linux/WSL 下手动编译 cache_src 子模块并链接的辅助脚本（与 Makefile 逻辑重复但更直白） |
| `compile_systemc.bat` | Windows 下从源码编译并安装 SystemC 2.3.3 的脚本 |
| `.gitignore` | 忽略 `build/` 目录和大量 `sim_*.log` / `build_*.log` 产物 |
| `README.md` | 项目说明（未包含依赖安装指引） |

## 3. 架构与约定

### 3.1 依赖声明方式
- 所有依赖在 `Makefile` 中以 **硬编码路径** 形式声明：
  - `SYSTEMC_PREFIX = /usr`
  - `SYSTEMC_INCLUDE = /usr/include`
  - `SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu`
  - 链接时 `-L$(SYSTEMC_LIB) -lsystemc -Wl,--no-as-needed -lpthread -lm`
- 头文件通过多个 `-I` 路径引入：`./iommu`、`./iommu/include`、`./iommu/iommu_fun_model`、`./iommu/iommu_perf_model`、`./iommu/cache_src` 及其子目录、`./slink`。
- 所有第三方库均为 **系统已安装的共享库**，不 vendoring、不 submodule、不 lockfile。

### 3.2 平台差异处理
- Linux/WSL：假设 SystemC 已通过包管理器或预编译安装到 `/usr` 下。
- Windows：通过 `compile_systemc.bat` 将 SystemC 源码（固定版本 `systemc-2.3.3`）编译到 `D:\TOOLS\systemc-2.3.3\install`，并通过 `setx` 设置环境变量；之后需重新打开终端使变量生效。
- 两个平台的 SystemC 安装路径不同，因此 `Makefile` 中的 `SYSTEMC_*` 变量需要按平台调整。

### 3.3 内部模块组织作为“内部依赖”
- 项目自身代码按功能划分为多个子目录：`iommu/iommu_fun_model`、`iommu/iommu_perf_model`、`iommu/cache_src/{cache,common,replacement,subsystem}`、`rp`、`pcienoc`、`slink`、`ddr`。
- `Makefile` 中的 `CXX_SOURCES` 列表显式枚举了每个 .cc/.cpp 文件，相当于把各子模块当作“内部依赖”进行手工组装。
- 新增源文件必须同步添加到 `CXX_SOURCES` 列表，否则不会被编译。

### 3.4 构建产物与清理
- 中间对象文件统一输出到 `build/` 目录（由 `build/%.o: %.cc` 规则生成）。
- `clean` 目标删除 `build/*.o`、`$(TARGET)` 以及整个 `build/` 目录。
- 大量 `build_*.log`、`sim_*.log` 文件存在于仓库根目录，表明实际开发中日志通常直接输出到工作区而非被 git 跟踪（`.gitignore` 仅忽略 `build/` 和部分 `*.log` 模式）。注意：根目录下存在大量 `build_*.log`、`sim_*.log` 文件未被 `.gitignore` 匹配，可能被提交到仓库。

## 4. 约定与约束

- **无包管理器**：仓库内不存在任何依赖声明文件（如 `go.mod`、`package.json`、`CMakeLists.txt`、`vcpkg.json`、`conan.lock`、`Cargo.lock` 等），所有第三方库依赖均通过系统路径和 `Makefile` 中的 `-I` / `-L` / `-l` 参数声明。
- **SystemC 版本锁定**：Windows 脚本 `compile_systemc.bat` 硬编码了 SystemC 源码路径 `D:\TOOLS\systemc-2.3.3\systemc-2.3.3`，即强制使用 SystemC 2.3.3 版本；Linux 端则依赖系统安装的 SystemC，版本由宿主系统决定，未在仓库中固化。
- **头文件路径约定**：所有项目头文件位于 `./iommu`、`./iommu/include`、`./iommu/iommu_fun_model`、`./iommu/iommu_perf_model`、`./iommu/cache_src` 及其子目录，编译时必须通过对应的 `-I` 标志暴露给编译器。
- **宏开关控制依赖行为**：通过 `TEST_FLAGS` 中的 `-DTEST_*` 宏（如 `-DTEST_TWO_STAGE`、`-DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3`、`-DSC_DISABLE_API_VERSION_CHECK`）在编译期切换功能分支，而不是通过外部配置文件或包管理器。
- **链接器参数约定**：始终使用 `-Wl,--no-as-needed` 确保 `libsystemc` 被动态加载，并使用 `-Wl,--allow-multiple-definition` 允许重复符号定义（SystemC TLM 常见需求）。
- **单元测试独立编译**：`test_dedup_unit` 目标直接用 `g++ test_dedup_prefetch_unit.cpp` 编译，不经过主 `Makefile` 的依赖图，属于轻量级独立测试。
- **无私有仓库/代理配置**：未发现任何 `GOPRIVATE`、npm registry、Conan remote、vcpkg registry 等私有源配置；依赖全部来自系统路径或公开源码。
- **无锁文件**：不存在 `*.lock`、`*.sum`、`*.json.lock` 等依赖版本锁定文件，依赖版本完全由系统环境和脚本中的硬编码路径决定。

## 5. 风险与建议

- 可移植性弱：SystemC 路径硬编码在 `Makefile` 和 Windows 脚本中，换机器/换系统需手动修改。
- 版本不可复现：Linux 端 SystemC 版本由宿主系统决定，无 lockfile 保证不同环境一致性。
- 维护成本：新增源文件需同时更新 `Makefile` 的 `CXX_SOURCES` 列表，容易遗漏。
- 建议：若团队规模扩大或跨平台需求增强，可考虑迁移到 CMake 以统一管理依赖路径、版本检查和多平台构建。