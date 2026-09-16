---
kind: dependency_management
name: 基于 SystemC 的 C++ 项目依赖管理（Makefile + 系统库链接）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - main.cpp
    - iommu/cache_config/default_config.json
    - iommu/cache_src/common/json_config.cpp
    - iommu/cache_src/common/json_config.h
    - iommu/cache_src/common/mini_json.h
---

## 1. 使用的系统与工具

本项目是一个纯 C++ SystemC 性能/功能模型，**没有使用任何包管理器**（无 `go.mod`、`package.json`、`CMakeLists.txt`、`vcpkg.json`、`conanfile.py` 等）。所有第三方依赖通过 **SystemC 运行时库** 以系统级静态/动态库形式引入，由根目录的 `Makefile` 和辅助脚本 `build_cpp.sh` 直接编译与链接。

- 编译器：`g++` / `gcc`，语言标准固定为 `-std=c++17`。
- 构建系统：GNU Make（`Makefile`），辅以若干 shell 脚本（`build_cpp.sh`、`compile_and_test.sh`、`link_and_run.sh`、`run_wsl.sh` 等）作为便捷入口。
- 外部依赖：仅依赖操作系统提供的 SystemC 库（`-lsystemc -lpthread -lm`）以及 POSIX 线程。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 主构建规则，声明所有源文件、头文件搜索路径、测试场景、调试宏、链接选项 |
| `build_cpp.sh` | 独立于 Make 的手动编译脚本，逐文件编译 `iommu/cache_src/*.cpp` 并链接 |
| `main.cpp` | 程序入口，链接到 SystemC 仿真内核 |
| `iommu/include/*.hh` | 项目内部公共接口头文件，被各模块包含 |
| `iommu/cache_src/common/json_config.h` | 解析 `default_config.json` 等配置文件的轻量 JSON 解析器（`mini_json.h`） |
| `iommu/cache_config/default_config.json` | 缓存子系统运行参数配置文件 |

## 3. 架构与约定

### 3.1 依赖来源
- **SystemC 运行时**：通过硬编码的系统路径引入——
  - `SYSTEMC_PREFIX = /usr`
  - `SYSTEMC_INCLUDE = /usr/include`
  - `SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu`
  - 链接时追加 `-L$(SYSTEMC_LIB) -lsystemc -Wl,--no-as-needed -lpthread -lm`
- **POSIX 线程与数学库**：`-lpthread -lm`。
- **无 vendoring**：项目中不存在 `vendor/`、`third_party/`、`lib/` 等子目录；所有第三方代码均以系统安装方式提供。

### 3.2 版本锁定策略
- **无 lockfile**：仓库中不存在 `*.lock`、`*.sum`、`Pipfile.lock`、`poetry.lock` 等同构文件。
- **版本约束方式**：完全依赖宿主系统的 SystemC 安装。构建脚本中通过编译期宏 `-DSC_DISABLE_API_VERSION_CHECK` 显式关闭 SystemC API 版本检查，使项目可兼容不同版本的 SystemC 安装。
- **可移植性约定**：若 SystemC 安装在非 `/usr` 路径，需修改 `Makefile` 中的 `SYSTEMC_*` 变量（见注释 `# adjust these paths if SystemC is installed elsewhere`）。

### 3.3 内部模块组织（自包含依赖）
项目自身按功能划分为多个子目录，彼此通过头文件包含而非共享库解耦：
- `iommu/iommu_fun_model/`：IOMMU 功能模型实现
- `iommu/iommu_perf_model/`：性能采集与统计
- `iommu/cache_src/{cache,common,replacement,subsystem}/`：PT/DC/PC/Walker/Msipt 缓存及替换策略
- `rp/`、`slink/`、`pcienoc/`、`ddr/`：各类测试驱动与外设模拟
- 所有 `.cc/.cpp` 源文件在 `Makefile` 的 `CXX_SOURCES` 中集中声明，统一编译到 `build/` 目录后链接为单一可执行文件 `iommu_model`。

### 3.4 构建产物与清理
- 对象文件输出至 `build/` 目录（`build/%.o`），最终产物为根目录下的 `iommu_model`。
- `make clean` 会删除 `build/` 目录及目标文件。
- 单元测试 `test_dedup_unit` 通过 `Makefile` 中的独立 target 用 `g++` 直接编译单个 `.cpp` 生成 `./test_dedup_unit`。

## 4. 约定与约束

- **必须使用 C++17**：所有编译命令均带 `-std=c++17`，禁止降级。
- **SystemC 必须已安装到系统路径**：默认期望位于 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`；若不在该位置，需手动修改 `Makefile` 中的 `SYSTEMC_*` 变量。
- **禁用 SystemC API 版本检查**：通过 `-DSC_DISABLE_API_VERSION_CHECK` 宏强制跳过版本校验，以便在不同 SystemC 发行版间复用同一份源码。
- **允许重复符号定义**：链接时使用 `-Wl,--allow-multiple-definition`，表明项目接受多 TU 中存在同名符号（通常用于单测或临时 hack）。
- **调试/发布开关**：通过 `DEBUG=0/1` 切换 `-O0 -g -DDEBUG*` 与 `-O3 -DNDEBUG` 两套编译标志。
- **测试场景通过 `TEST=` 选择**：`Makefile` 根据 `TEST` 变量选择对应的 `rp/test_rp_*.cc` 测试源文件和一组 `TEST_CFG_*` 编译期宏，无需重新维护源码即可切换行为。
- **配置文件与源码分离**：缓存相关参数集中在 `iommu/cache_config/default_config.json`，由 `json_config.cpp` 在运行时读取，避免硬编码。

## 5. 总结

该项目采用最简化的 C++ 构建方式：**Makefile + 系统级 SystemC 库**，不引入任何包管理器、锁文件或私有仓库。依赖管理的核心就是确保宿主环境正确安装了 SystemC 及其头文件/库，并通过 `SYSTEMC_*` 变量指向其安装路径。升级第三方依赖等同于升级系统安装的 SystemC 版本，并在必要时调整 `SYSTEMC_*` 路径。