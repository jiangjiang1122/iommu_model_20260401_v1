---
kind: build_system
name: 基于 Makefile + Shell 脚本的 SystemC 模型构建与测试体系
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
    - build_wsl.sh
    - compile_wsl.sh
    - run_wsl.sh
    - test_integration_dedup_prefetch.sh
    - main.cpp
---

## 1. 使用的系统/方法

本项目是一个 RISC-V IOMMU 的 SystemC 行为/性能混合模型，构建系统围绕 **GNU Make + Bash 脚本** 组织，依赖系统安装的 SystemC（`/usr/include`、`/usr/lib/x86_64-linux-gnu`），通过 `g++ -std=c++17` 编译，链接时额外需要 `-lsystemc -lpthread -lm`。

没有使用 CMake、Meson、Bazel 等现代构建工具，也没有 Dockerfile / CI 配置；所有构建逻辑集中在根目录的 `Makefile` 和若干 `.sh` 辅助脚本中。

## 2. 关键文件

- `Makefile`：核心构建入口，定义编译器、SystemC 路径、调试/发布开关、全部源文件列表、目标产物 `iommu_model`，以及多场景测试 target。
- `build_cpp.sh`：独立于 Make 的手动编译脚本，遍历 `iommu/cache_src/**/*.cpp` 逐个编译为 `build/` 下对象文件后统一链接。
- `compile_and_test.sh`：增量编译 `pt_cache.cpp` 与 `iommu_top.cc` 并运行模型的便捷脚本。
- `link_and_run.sh`、`run_wsl.sh`、`build_wsl.sh`、`compile_wsl.sh`、`test_*`、`check_*`、`extract_*`、`analyze_*`：大量位于根目录的 Shell 脚本，用于不同平台（WSL/Linux）、不同测试场景的编译、运行、结果提取与分析。
- `build/`：统一的中间对象输出目录（由 Makefile 规则 `build/%.o: %.cc` 生成）。
- `main.cpp`：可执行程序的入口点。

## 3. 架构与约定

### 3.1 编译流程

1. **选择测试场景**：通过 `make TEST=<scenario>` 指定测试线程源文件（如 `rand4k_singlestage`、`seq128k_twostage`、`sv48_bare`、`virt_lazy_twostage`、`cache_inval` 等），Makefile 中的 `ifeq` 分支将 `TEST_THREAD_SRC` 指向 `rp/test_rp_*.cc` 中的一个，并注入对应的 `TEST_FLAGS`（宏定义，如 `-DTEST_TWO_STAGE -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3`）。
2. **收集源文件**：`CXX_SOURCES` 显式列出所有被编译的 `.cc/.cpp` 文件，包括 `iommu/iommu_fun_model`、`iommu/iommu_perf_model`、`iommu/cache_src/*`、`rp/*`、`pcienoc/*`、`slink/*`、`ddr/*` 下的实现文件，最后拼接 `$(TEST_THREAD_SRC)`。
3. **对象文件输出**：所有 `.o` 放在 `build/<原路径>/` 下，保持源码树结构，避免污染源码目录。
4. **链接**：最终产物为根目录的 `iommu_model`，链接参数固定为 `-L$(SYSTEMC_LIB) -lsystemc -Wl,--no-as-needed -lpthread -lm -Wl,--allow-multiple-definition`。
5. **清理**：`make clean` 删除 `build/*.o` 和 `build/` 目录。

### 3.2 调试/发布模式

- `DEBUG=1`（默认）：开启 `-g -O0` 及大量 `DEBUG_*` 宏（`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_MSITRANS`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS`）。
- `DEBUG=0`：开启 `-O3 -DNDEBUG`。

### 3.3 SystemC 集成

- 头文件路径包含 `-I/usr/include` 以及项目内各子模块 include 路径。
- 启用 SystemC 动态进程：`-DSC_INCLUDE_DYNAMIC_PROCESSES`。
- 关闭 API 版本检查：`-DSC_DISABLE_API_VERSION_CHECK`，以兼容不同 SystemC 安装版本。

### 3.4 测试与验证

- 单元测试：`make test_dedup_unit` 直接编译 `test_dedup_prefetch_unit.cpp` 并运行。
- 集成测试：`make test_dedup_integration` 先构建主目标，再调用 `test_integration_dedup_prefetch.sh`。
- 场景化测试：通过 `make TEST=...` 切换不同负载（单级/两级翻译、随机/顺序、虚拟化 Lazy/Strict 失效、大页 2MB、高带宽 128GB/s 等），每个场景在 Makefile 中都有完整注释说明预期行为和调参方式。
- 结果分析：`tmp/` 目录下有大量 Python 脚本（`analyze_exit_iops.py`、`analyze_pipeline_stages.py`、`calc_iops.sh`、`extract_stats.sh` 等）配合 Shell 脚本对模型输出进行统计、绘图。

### 3.5 跨平台/环境

- 提供 `build_wsl.sh`、`compile_wsl.sh`、`run_wsl.sh` 等 WSL 专用脚本，硬编码 Windows 路径 `/mnt/d/...`，表明开发环境主要在 Windows 上通过 WSL 构建 Linux 二进制。
- `compile_systemc.bat` 是 Windows 批处理脚本，可能用于在原生 Windows 环境下编译 SystemC 本身。

## 4. 约定与约束

- **源文件必须显式加入 `CXX_SOURCES`**：新增 `.cc/.cpp` 不会自动被发现，必须在 Makefile 中添加一行，否则不会被编译。
- **测试场景通过 `TEST=` 变量选择**：新增测试需遵循现有 `ifeq ($(TEST), ...)` 分支模式，设置 `TEST_THREAD_SRC` 和 `TEST_FLAGS`。
- **对象文件统一输出到 `build/`**：构建规则要求 `mkdir -p $(dir $@)` 保证目录存在，禁止直接输出到源码目录。
- **SystemC 安装位置硬编码为 `/usr`**：若 SystemC 安装在其他位置，需修改 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB`。
- **链接允许重复符号**：始终传递 `-Wl,--allow-multiple-definition`，说明项目中存在多处同名符号定义（可能是 SystemC 或第三方库的 ODR 问题 workaround）。
- **调试宏驱动功能开关**：大量 `DEBUG_*` 宏同时控制编译优化级别和运行时调试输出，调试与发布构建差异显著。
- **无自动化 CI**：仓库中没有 GitHub Actions、GitLab CI、Jenkins 等配置文件，构建与测试完全依赖本地 Makefile 和 Shell 脚本手动执行。