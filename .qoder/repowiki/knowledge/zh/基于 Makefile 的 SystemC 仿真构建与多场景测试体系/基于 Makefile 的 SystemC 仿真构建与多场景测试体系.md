---
kind: build_system
name: 基于 Makefile 的 SystemC 仿真构建与多场景测试体系
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - compile_wsl.sh
    - run_wsl.sh
    - compile_systemc.bat
---

## 1. 构建系统概述
该项目采用 **GNU Make** 作为核心构建工具，配合 **g++ (C++17)** 编译器，用于编译和链接基于 **SystemC/TLM** 的 RISC-V IOMMU 硬件仿真模型。构建流程高度依赖环境变量和预处理器宏（如 `-DDEBUG`）来控制仿真行为的精细度。

## 2. 核心构建逻辑
- **入口文件**: `main.cpp` 是仿真的唯一入口，负责实例化顶层模块（`iommu_top`, `DDR_Module`, `RP_Module` 等）并绑定 TLM Socket。
- **源文件管理**: `Makefile` 显式列出了所有 `.cc` 和 `.cpp` 源文件，涵盖功能模型（`iommu_fun_model`）、性能模型（`iommu_perf_model`）、缓存子系统（`cache_src`）以及外围接口模拟（`rp`, `pcienoc`, `slink`, `ddr`）。
- **输出目录**: 所有中间目标文件（`.o`）统一存放在 `build/` 目录下，保持源码目录整洁。
- **链接配置**: 链接阶段需指定 SystemC 库路径（默认 `/usr/lib/x86_64-linux-gnu`），并处理多线程支持（`-lpthread`）及符号重定义问题（`--allow-multiple-definition`）。

## 3. 多场景测试驱动 (Test Scenarios)
构建系统通过 `TEST` 变量支持多种预设的仿真场景，自动切换对应的测试线程源文件和编译标志：
| 场景标识 | 描述 | 关键宏定义 |
| :--- | :--- | :--- |
| `rand4k_singlestage` | 4KB 随机读 + 单级翻译 (默认) | - |
| `seq128k_singlestage` | 128KB 顺序读 + 单级翻译 | - |
| `sv48_bare` | Sv48 + Bare 基础测试 | - |
| `seq128k_twostage` | 128KB 顺序读 + 两阶段翻译 | `-DTEST_SEQ_128K`, `-DTEST_TWO_STAGE` |

此外，还提供了专门的单元测试目标 `test_dedup_unit`，用于独立验证 PT Cache 的去重与预取逻辑。

## 4. 跨平台与环境适配
- **WSL/Linux**: 提供了 `compile_wsl.sh` 和 `run_wsl.sh` 脚本，简化在 Windows Subsystem for Linux 下的编译与运行流程。
- **Windows**: 包含 `compile_systemc.bat`，用于在 Windows 环境下从源码编译 SystemC 库并配置环境变量。
- **调试支持**: 通过 `DEBUG=1` 开关可开启大量细粒度的调试宏（如 `DEBUG_TRANSLATION`, `DEBUG_TWOSTAGE`），便于追踪地址翻译流水线的内部状态。

## 5. 开发者规范
- **新增源文件**: 若添加新的 `.cc` 或 `.cpp` 文件，必须同步更新 `Makefile` 中的 `CXX_SOURCES` 列表。
- **环境路径**: 确保 SystemC 安装在 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`，或通过修改 `Makefile` 中的 `SYSTEMC_PREFIX` 指向自定义路径。
- **清理构建**: 建议使用 `make clean` 或 `make rebuild` 以避免因头文件变更导致的增量编译错误。