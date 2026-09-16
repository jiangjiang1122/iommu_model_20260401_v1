---
kind: dependency_management
name: 基于系统包管理器与硬编码路径的 SystemC/C++ 依赖管理
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - README.md
    - PERF_MODEL_IMPLEMENTATION.md
    - compile_wsl.sh
    - link_and_run.sh
---

## 1. 使用的系统与工具

本项目是一个纯 C++/SystemC 仿真模型，**没有使用任何语言级包管理器**（无 `go.mod`、`package.json`、`Cargo.toml`、`CMakeLists.txt`、`vcpkg.json`、`conanfile.py` 等）。所有第三方依赖通过操作系统包管理器安装后，由 Makefile 中的硬编码路径直接链接。

- **构建系统**：GNU Make（根目录 `Makefile`）
- **编译器**：g++ / gcc，要求 C++17（`-std=c++17`）
- **运行时库**：SystemC（`-lsystemc`）、pthread、math（`-lm`）
- **运行环境约束**：README 明确要求“必须在 WSL2 + Ubuntu 环境中编译和运行”，SystemC 需预装于 `/usr` 目录下。

## 2. 关键文件

- `Makefile`：唯一的构建入口，集中声明了 SystemC 的头文件路径、库路径、编译选项、源文件列表和目标可执行文件。
- `build_cpp.sh`：辅助脚本，手动遍历 `iommu/cache_src` 下的 `.cpp` 文件逐个编译并链接，作为 Make 之外的备选构建方式。
- `compile_wsl.sh`、`compile_systemc.bat`、`build_wsl.sh`、`link_and_run.sh`：面向不同平台/场景的便捷脚本，均复用 Makefile 或硬编码 g++ 命令。
- `README.md`：文档化依赖要求（WSL2、Ubuntu、SystemC 2.3.x、GCC/G++、GNU Make）。
- `PERF_MODEL_IMPLEMENTATION.md`：记录实际使用的 SystemC 版本为 `2.3.3-Accellera`。

## 3. 架构与约定

### 3.1 外部依赖定位策略

SystemC 以**系统级共享库**形式存在，路径在 Makefile 中硬编码：

```make
SYSTEMC_PREFIX = /usr
SYSTEMC_INCLUDE = /usr/include
SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu
LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm
```

这意味着：
- 依赖不在仓库内（无 `vendor/`、`third_party/`、`lib/` 子目录）。
- 依赖版本不由项目锁定；构建成功与否完全取决于宿主机器上 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu` 是否存在匹配的 SystemC 头文件和库文件。
- 文档仅给出范围约束（`SystemC 2.3.x`），未提供精确版本锁定机制（如 lockfile）。

### 3.2 内部模块组织即“依赖图”

由于没有包管理器，项目的“内部依赖关系”通过 Makefile 中的 `CXX_SOURCES` 列表显式声明。每个 `.cc/.cpp` 文件被逐一加入编译单元，最终链接为单一可执行文件 `iommu_model`。这种扁平化的源文件集合本身就是项目的依赖拓扑——新增模块必须手动添加到 `CXX_SOURCES` 列表中，否则不会被纳入构建。

### 3.3 多测试场景通过宏开关组合

不同测试场景（单级/两级地址翻译、虚拟化 Lazy/Strict 模式、缓存失效注入等）不是通过不同的依赖集实现，而是通过 `TEST` 变量选择对应的 `rp/test_rp_*_thread.cc` 源文件，并通过 `-DTEST_*` 编译期宏切换行为。这相当于用编译期配置替代了依赖版本管理。

## 4. 约定与约束

- **必须使用 WSL2 + Ubuntu**：README 明确禁止在 Windows 原生环境下编译，因为 SystemC 的安装位置与路径假设仅在 Linux 下成立。
- **SystemC 必须已安装到 `/usr`**：Makefile 中 `SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB` 是固定值，未提供 `--with-systemc-prefix` 之类的可配置参数；若 SystemC 安装在其他位置，需手动修改 Makefile。
- **无依赖版本锁定**：仓库不包含任何 lockfile（如 `go.sum`、`package-lock.json`、`Pipfile.lock`、`requirements.txt` 等），也不包含 vendored 源码。依赖来源完全依赖宿主系统的包管理器（如 `apt install systemc`）。
- **无私有注册表或镜像源**：未发现任何 `GOPRIVATE`、`.npmrc`、`pip.conf`、`~/.m2/settings.xml` 等私有源配置。
- **构建产物隔离**：中间对象文件输出到 `build/` 目录，可执行文件位于根目录；`clean` 目标会删除 `build/*.o` 和 `$(TARGET)`。
- **调试/发布双模式**：通过 `DEBUG=1|0` 控制是否启用 `-g -O0 -DDEBUG` 及大量 `DEBUG_*` 宏，用于切换功能模型的详细日志输出。
- **允许重复符号**：链接时使用了 `-Wl,--allow-multiple-definition`，说明多个源文件中可能存在同名符号（可能是 SystemC TLM 模板实例化导致的常见现象），这是当前构建约定的一个特殊约束。

## 5. 总结

该项目采用最传统的 C++ 构建方式：**无包管理器、无锁文件、无 vendoring**。所有依赖（SystemC、pthread、math）均以系统库形式存在，通过 Makefile 中的硬编码路径引用。依赖的版本与安装位置由使用者自行保证，仓库本身不追踪、不锁定、不分发任何第三方代码。对于需要跨机器复现构建的场景，当前方案依赖使用者遵循 README 中的环境要求手动安装 SystemC 2.3.x。