---
kind: build_system
name: 基于 Makefile 与 Shell 脚本的 SystemC IOMMU 仿真构建体系
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
    - run_report.sh
    - test_samples.sh
    - build_wsl.sh
    - compile_wsl.sh
    - run_wsl.sh
    - compile_systemc.bat
---

## 1. 构建系统概览

本项目是一个基于 SystemC/TLM 的 RISC-V IOMMU 性能模型与验证平台，采用 **GNU Make + Bash 脚本** 的传统 C++ 构建方式，没有使用 CMake、Meson、Bazel 等现代构建系统。所有编译、链接、测试、运行流程均由根目录的 `Makefile` 和多个 `.sh` 脚本协作完成。

- **编译器**: `g++` (C++17, `-std=c++17`)，辅助 `gcc`。
- **依赖库**: SystemC (`-lsystemc`)、POSIX 线程 (`-lpthread`)、数学库 (`-lm`)。
- **SystemC 安装路径**: 默认指向 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`，通过 `SYSTEMC_PREFIX` / `SYSTEMC_INCLUDE` / `SYSTEMC_LIB` 变量可调整。
- **输出产物**: 单一可执行文件 `iommu_model`，由 `main.cpp` 作为 SystemC 仿真入口（`sc_main`）。

## 2. 核心构建文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 主构建规则，定义源码列表、目标、测试场景选择、调试/发布模式切换 |
| `build_cpp.sh` | 独立编译 `cache_src` 子模块并链接的便捷脚本 |
| `compile_and_test.sh` | 增量编译指定源文件并链接运行的脚本 |
| `link_and_run.sh` | 仅链接已编译对象并运行仿真 |
| `run_report.sh` | 带超时运行仿真并过滤关键统计输出 |
| `test_samples.sh` | 编译特定模块并运行以查看延迟采样 |
| `build_wsl.sh` / `compile_wsl.sh` / `run_wsl.sh` | WSL 环境专用构建/运行脚本 |
| `compile_systemc.bat` | Windows 批处理脚本（用于 SystemC 自身编译） |

## 3. 构建架构与约定

### 3.1 测试场景驱动构建
`Makefile` 通过 `TEST` 变量（默认 `rand4k_singlestage`）选择不同测试场景，每个场景对应一个 `rp/test_rp_*.cc` 线程源文件，并通过 `TEST_FLAGS` 注入大量编译期宏参数（如 `-DTEST_TWO_STAGE -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1` 等）。新增测试只需在 `ifeq ($(TEST), ...)` 分支中添加配置即可。

### 3.2 源码组织与编译单元
- 公共源码集中在 `iommu/iommu_fun_model/`、`iommu/iommu_perf_model/`、`iommu/cache_src/`（含 cache、replacement、subsystem、common 子目录）、`rp/`、`pcienoc/`、`slink/`、`ddr/`。
- 所有 `.cc`/`.cpp` 统一编译为 `build/<path>.o` 形式的对象文件，保持与源码树对应的目录结构。
- 头文件通过 `-I` 参数显式包含：`./iommu`, `./iommu/include`, `./iommu/iommu_fun_model`, `./iommu/iommu_perf_model`, `./iommu/cache_src` 及其子目录。

### 3.3 调试与发布模式
- `DEBUG=1`（默认）: `-g -O0 -DDEBUG`，并自动开启一系列 `DEBUG_*` 宏（`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_MSITRANS`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS`）。
- `DEBUG=0`: `-O3 -DNDEBUG`，关闭调试信息。
- 可通过 `EXTRA_FLAGS` 或 `TEST_FLAGS` 追加自定义编译选项。

### 3.4 链接选项
- 强制链接 SystemC 动态库：`-L$(SYSTEMC_LIB) -lsystemc -Wl,--no-as-needed`。
- 允许多重符号定义：`-Wl,--allow-multiple-definition`（因 SystemC 某些实现特性）。
- 链接顺序：先收集 `build/` 下所有 `.o`，再链接到 `iommu_model`。

### 3.5 仿真入口与模块绑定
`main.cpp` 中创建 `iommu_top`、`DDR_Module`、`RP_Module`、`PCIENOC_Module`、`SLINK_Module` 实例，并通过 TLM socket 进行端口绑定，最后调用 `sc_start(300000000, SC_NS)` 运行 300ms 仿真。

## 4. 测试与验证流程

- **单元测试**: `make test_dedup_unit` 单独编译 `test_dedup_prefetch_unit.cpp` 生成 `test_dedup_unit` 并运行。
- **集成测试**: `make test_dedup_integration` 依赖 `iommu_model` 目标，然后执行 `test_integration_dedup_prefetch.sh`。
- **场景化测试**: 通过 `make TEST=<scenario>` 选择不同负载（单级/两级翻译、随机/顺序、MSI混合、虚拟化 Lazy/Strict 失效等），每种场景有独立的线程源文件和参数宏集。
- **结果分析**: `tmp/*.py` 和 `tmp/*.sh` 提供大量 Python/Shell 分析脚本，用于解析仿真输出、统计 IOPS、PTW 并发、DDR 访问间隔等。

## 5. 约束与注意事项

- SystemC 必须预先安装到 `/usr` 前缀路径，或通过修改 `SYSTEMC_INCLUDE` / `SYSTEMC_LIB` 指向自定义安装位置。
- 项目未使用版本控制外的构建缓存机制（如 ccache），每次 `clean` 会删除整个 `build/` 目录。
- 所有构建脚本均硬编码绝对路径 `/mnt/d/Qoder_proj/iommu_model_20260401_v1`，表明当前主要在 WSL 环境下开发。
- 不存在 Dockerfile、CI/CD 配置文件（如 GitHub Actions、Jenkinsfile），也没有自动化发布流水线；构建完全依赖本地手动执行。
- 通过 `SCENE*_PTW`、`VIRT_FQ_DEPTH`、`VIRT_TRAP_NS` 等 make 变量可在命令行覆盖测试参数，无需修改源码。

## 6. 关键文件清单

- `Makefile` — 主构建规则
- `main.cpp` — SystemC 仿真入口
- `build_cpp.sh` — cache_src 模块编译脚本
- `compile_and_test.sh` — 增量编译+运行脚本
- `link_and_run.sh` — 链接+运行脚本
- `run_report.sh` — 带超时仿真+日志过滤
- `test_samples.sh` — 样本测试脚本
- `build_wsl.sh` / `compile_wsl.sh` / `run_wsl.sh` — WSL 专用脚本
- `compile_systemc.bat` — Windows 下 SystemC 编译脚本
- `tmp/*.py` / `tmp/*.sh` — 仿真结果分析与统计脚本
- `rp/test_rp_*.cc` — 各测试场景驱动源文件