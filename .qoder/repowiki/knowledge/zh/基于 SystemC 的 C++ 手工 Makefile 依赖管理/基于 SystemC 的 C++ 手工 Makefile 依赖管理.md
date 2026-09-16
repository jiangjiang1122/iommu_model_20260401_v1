---
kind: dependency_management
name: 基于 SystemC 的 C++ 手工 Makefile 依赖管理
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - README.md
    - .gitignore
---

## 1. 使用的系统/方法

本项目是一个纯 C++ SystemC 仿真工程，**没有使用任何包管理器**（无 `package.json`、`go.mod`、`Cargo.toml`、`CMakeLists.txt`、`vcpkg.json`、`conan`、`meson` 等）。所有第三方依赖通过 **手工维护的 Makefile + Shell 脚本** 直接链接到系统已安装的库。

- 唯一外部依赖是 **SystemC 2.3.x**（见 README.md “环境要求”），以静态/动态库形式安装到宿主 Linux 系统的 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`。
- 构建工具链为 `g++` (C++17) + GNU `make` + `pthread` + `m`。
- Windows 侧提供 `compile_systemc.bat`，用 CMake 从源码编译并安装 SystemC 2.3.3 到 `D:\TOOLS\systemc-2.3.3\install`，设置 `SYSTEMC_HOME / SYSTEMC_INCLUDE / SYSTEMC_LIBDIR` 环境变量。
- WSL/Linux 侧通过根目录 `Makefile` 和 `build_cpp.sh`、`compile_wsl.sh`、`build_wsl.sh` 等脚本调用系统路径下的 SystemC。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 唯一构建入口：声明编译器、SystemC 头/库路径、全部源文件列表、各测试场景宏开关、链接选项 |
| `build_cpp.sh` | 逐个编译 `iommu/cache_src/*.cpp` 并链接，作为 Makefile 之外的备选构建方式 |
| `compile_systemc.bat` | Windows 下从源码编译安装 SystemC 2.3.3 |
| `README.md` | 文档化“必须在 WSL 中运行”“SystemC 安装在 `/usr`”等前置约束 |
| `.gitignore` | 忽略 `build/` 产物与大量 `sim_*.log`、`build_*.log` 输出 |

## 3. 架构与约定

- **依赖声明位置集中**：所有依赖信息集中在 `Makefile` 前 40 行——`SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB`、`LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm`。修改 SystemC 安装位置只需改这几处变量。
- **源文件清单硬编码**：`CXX_SOURCES` 变量显式列出 `iommu/iommu_fun_model/*`、`iommu/iommu_perf_model/*`、`iommu/cache_src/*`、`rp/*`、`pcienoc/*`、`slink/*`、`ddr/*` 下的每个 `.cc/.cpp` 文件。新增模块需手动加入该列表。
- **多场景通过宏切换**：不同测试场景（单级/两级、随机/顺序、虚拟化 lazy/strict、MSI 混合等）不靠子项目或子仓库，而是通过 `TEST=...` 选择 `TEST_THREAD_SRC` 并追加对应的 `-DTEST_*` 编译期宏，由同一份代码编译出不同行为的可执行文件。
- **SystemC 版本锁定**：Windows 脚本固定 `systemc-2.3.3`；Linux 侧通过 `#include <systemc>` 与 `-lsystemc` 链接系统库，未记录具体版本号，但 README 指定 “SystemC 2.3.x”。
- **构建产物隔离**：所有 `.o` 放入 `build/` 目录，可执行文件 `iommu_model` 放在根目录；`clean` 目标清理 `build/*.o` 与 `$(TARGET)`。

## 4. 约定与约束

- **必须在 WSL/Linux 环境中编译和运行**（README.md 明确说明），因为 SystemC 库位于 `/usr` 且脚本硬编码了 Linux 路径。
- **SystemC 必须预先安装到系统路径**：`/usr/include` 与 `/usr/lib/x86_64-linux-gnu`，否则 `make all` 会因找不到 `<systemc>` 头文件或无法链接 `-lsystemc` 失败。
- **禁止混用包管理器**：仓库内不存在任何依赖描述文件（`package.json`、`go.mod`、`Pipfile`、`requirements.txt`、`CMakeLists.txt` 等），因此不存在“更新依赖”的标准流程；升级 SystemC 需要人工修改 `Makefile` 中的路径或重新安装到 `/usr`。
- **无锁文件/供应商目录**：没有 `vendor/`、`third_party/`、`lockfile`，所有源码与依赖均假定已在宿主机就绪。
- **链接选项固定**：统一使用 `-Wl,--no-as-needed -lpthread -lm -Wl,--allow-multiple-definition`，后者用于容忍 SystemC 动态库的多重定义符号。
- **调试/发布模式二选一**：`DEBUG=1`（默认，加 `-g -O0` 及大量 `DEBUG_*` 宏）与 `DEBUG=0`（`-O3 -DNDEBUG`）通过同一个 Makefile 切换。
- **单元测试独立目标**：`test_dedup_unit` 直接用 `g++ test_dedup_prefetch_unit.cpp` 编译，不经过主构建管线，属于临时验证手段。