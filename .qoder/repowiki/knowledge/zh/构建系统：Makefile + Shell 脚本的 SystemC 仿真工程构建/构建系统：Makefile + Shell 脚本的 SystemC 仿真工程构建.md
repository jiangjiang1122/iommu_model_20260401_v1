---
kind: build_system
name: 构建系统：Makefile + Shell 脚本的 SystemC 仿真工程构建
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
    - build_wsl.sh
    - compile_wsl.sh
---

## 1. 使用的系统与工具
- **构建系统**：GNU Make（`Makefile`）作为核心构建编排，配合多个 Bash 脚本完成编译、链接、运行与测试。
- **编译器**：g++ / gcc，C++ 标准固定为 C++17（`-std=c++17`），关闭警告（`-w`）。
- **仿真框架**：SystemC/TLM 2.0，通过 `-lsystemc -L/usr/lib/x86_64-linux-gnu` 链接系统安装的 SystemC 库。
- **依赖库**：pthread、math（`-lpthread -lm`）。
- **输出产物**：可执行文件 `iommu_model`，对象文件统一输出到 `build/` 目录。

## 2. 关键文件与包
- `Makefile`：全部构建规则、目标、测试场景选择、编译/链接选项集中定义。
- `main.cpp`：SystemC 仿真入口，实例化 IOMMU、DDR、RP、PCIENOC、SLINK 模块并绑定 TLM Socket。
- `build_cpp.sh`：直接遍历 cache_src 下的 .cpp 文件逐个编译再统一链接的简化脚本。
- `compile_and_test.sh` / `link_and_run.sh`：分步编译+链接+运行的一键脚本。
- `build_wsl.sh` / `compile_wsl.sh`：WSL 环境专用构建脚本，将工程复制到 WSL 本地文件系统后调用 `make`，再将二进制拷回 Windows 目录。
- `tmp/*.sh`：大量分析/回归脚本（如 `run_baseline_regression.sh`、`run_walker_regression.sh`、`extract_stats.sh` 等），用于自动化测试与性能数据采集。

## 3. 架构与约定
- **单一顶层 Makefile 驱动**：所有源文件在 `CXX_SOURCES` 中显式列出，对象文件通过 `patsubst` 映射到 `build/<path>.o`，保持源码树与构建产物分离。
- **测试场景参数化**：通过 `TEST=<scenario>` 变量切换不同测试线程源文件（`rp/test_rp_*.cc`），并为每个场景注入一组 `TEST_CFG_*` 宏（如 `TEST_TWO_STAGE`、`TEST_SEQ_128K`、`TEST_RAND_4K`、`TEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3`、`TEST_CFG_WALKER_CACHE_S2_ENABLED=1` 等），实现同一套代码的多配置编译。
- **调试/发布模式**：`DEBUG=1`（默认）启用 `-g -O0` 及大量 `DEBUG_*` 宏；`DEBUG=0` 使用 `-O3 -DNDEBUG` 发布优化。
- **SystemC 动态进程**：强制开启 `-DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK`，以兼容特定 SystemC 版本。
- **链接选项**：统一添加 `-Wl,--no-as-needed -Wl,--allow-multiple-definition`，解决多定义符号与动态库依赖问题。
- **跨平台构建**：`build_wsl.sh` 封装了“复制→WSL 本地编译→拷回二进制”的完整流程，避免 WSL 文件系统性能问题。

## 4. 约定与约束
- **源文件组织**：IOMMU 功能模型位于 `iommu/iommu_fun_model/`，性能模型位于 `iommu/iommu_perf_model/`，缓存子系统位于 `iommu/cache_src/`，测试线程位于 `rp/`，外设模型位于 `ddr/`、`pcienoc/`、`slink/`。Makefile 中已硬编码这些路径，新增源文件需同步更新 `CXX_SOURCES`。
- **头文件包含路径**：`SYSTEMC_INCLUDE` 指向 `/usr/include`，项目内 include 路径通过 `-I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src ...` 逐一声明，未出现在其中的头文件会导致编译失败。
- **测试场景命名规范**：`TEST=` 值必须匹配 Makefile 中 `ifeq ($(TEST), ...)` 分支之一，否则回退到默认的 `rand4k_singlestage`。
- **构建产物位置**：所有 `.o` 文件必须落在 `build/` 下，`clean` 目标会删除整个 `build/` 目录，确保增量构建干净。
- **SystemC 仿真时间**：`main.cpp` 中 `sc_start(300000000, sc_core::SC_NS)` 固定运行 300ms，修改测试时长需直接编辑该文件。
- **WSL 构建约束**：`build_wsl.sh` 要求工程路径为 `/mnt/d/Qoder_proj/iommu_model_20260401_v1`，且 WSL 侧临时目录为 `/tmp/iommu_build`，路径变更需同步修改脚本。
- **无 CI/Docker**：仓库未提供 Dockerfile 或 GitHub Actions/GitLab CI 配置，构建完全依赖本地 `make` 与 Shell 脚本。
