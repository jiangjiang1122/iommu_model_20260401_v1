---
kind: build_system
name: 基于Makefile的SystemC仿真模型构建与测试体系
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
    - run_report.sh
    - test_dedup_prefetch.sh
---

## 1. 构建系统概览

本项目是一个基于 SystemC/TLM 的 RISC-V IOMMU 性能与功能仿真模型，采用 **GNU Make + Shell 脚本** 的轻量级构建体系，目标产物为单一可执行文件 `iommu_model`。构建依赖外部安装的 SystemC（默认路径 `/usr/include`、`/usr/lib/x86_64-linux-gnu`），使用 `g++` 编译 C++17 源码。

## 2. 核心构建文件

- **`Makefile`**：主构建入口，定义编译器、包含路径、调试/发布模式、链接选项及所有测试场景。
- **`main.cpp`**：SystemC 仿真顶层，实例化 IOMMU、DDR、RP、PCIENOC、SLINK 模块并通过 TLM socket 绑定。
- **`build_cpp.sh`**：手动编译 cache_src 子目录并链接的辅助脚本。
- **`compile_and_test.sh` / `link_and_run.sh`**：增量编译与运行脚本。
- **`build_wsl.sh` / `compile_wsl.sh`**：WSL 环境专用构建脚本，将工程复制到 WSL 本地文件系统编译后拷回 Windows。
- **`run_report.sh`**：运行仿真并过滤关键统计输出（IOPS、Efficiency、PTW、DDR latency 等）。
- **`test_dedup_prefetch.sh`**：针对 PT Cache 去重+预取功能的自动化测试脚本，通过 grep 分析日志验证行为。

## 3. 构建架构与约定

### 3.1 编译配置
- 编译器：`g++`，标准 `c++17`，启用 `-std=c++17 -w`。
- 包含路径：除当前目录外，显式指定 `./iommu`、`./iommu/include`、`./iommu/iommu_fun_model`、`./iommu/iommu_perf_model`、`./iommu/cache_src` 及其子目录 `cache`、`common`、`replacement`、`subsystem`、`./slink`。
- SystemC 宏：`-DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK`。
- 链接库：`-lsystemc -lpthread -lm`，并使用 `-Wl,--no-as-needed -Wl,--allow-multiple-definition` 解决多定义问题。
- 对象文件统一输出到 `build/` 目录，按源文件路径镜像组织。

### 3.2 调试/发布模式
通过 `DEBUG=0/1` 切换：
- Debug（默认 `DEBUG=1`）：`-g -O0`，并自动开启大量 `DEBUG_*` 宏（`DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_MSITRANS`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS`）。
- Release：`-O3 -DNDEBUG`。

### 3.3 测试场景选择机制
通过 `make TEST=<scenario>` 选择不同测试线程源文件和编译标志，由 Makefile 中的 `ifeq` 分支决定：
- `rand4k_singlestage`（默认）：4KB 随机读 + 单级地址翻译。
- `seq128k_singlestage`：128KB 顺序读 + 单级翻译。
- `sv48_bare`：Sv48 + Bare 基础测试。
- `seq128k_twostage` / `rand4k_twostage`：两阶段地址翻译，启用 PT Dedup Prefetch Depth=3、Walker Cache。
- `seq512b_2mb_twostage_s2on[_128g]`：大页（2MB）+ S2 开启 + 高带宽端口（512bit/1024bit）。
- `virt_lazy_twostage` / `virt_strict_twostage`：虚拟化两级 Stage 的 Lazy/Strict 失效模式。
- `cache_inval`：缓存失效功能测试。
每个场景通过 `TEST_FLAGS` 注入不同的 `TEST_CFG_*` 宏（如 `TEST_CFG_AXI_PORT_WIDTH_BIT`、`TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING`、`TEST_CFG_PTW_MAX_OUTSTANDING_TASKS`、`TEST_CFG_SKIP_PHASE1`、`TEST_CFG_NUM_PAGES`、`TEST_CFG_INVAL_PERIOD_REQS`、`TEST_CFG_VIRT_FQ_DEPTH` 等）来配置仿真参数。

### 3.4 源码组织与构建映射
- `iommu/iommu_fun_model/*.cc`：功能模型实现。
- `iommu/iommu_perf_model/*.cc`：性能模型实现（命令队列、设备上下文、HPM、参考 API、PTW、重排序、xDTW、PT Cache 响应、MSIPT Cache、Forwarder/Fault/CQ、任务缓存转换等）。
- `iommu/cache_src/{cache,common,replacement,subsystem}/*.cpp`：多级缓存子系统（DC/PC/PT/Walker/MSIPT/Dedup Cache、PLRU/SRIP 替换策略、JSON 配置、统计收集器）。
- `rp/test_rp_*.cc`：各测试场景的驱动线程。
- `pcienoc/test_pcienoc.cc`、`slink/test_slink.cc`、`ddr/test_ddr.cc`：外设/NoC/内存模型。

### 3.5 单元测试与集成测试
- `make test_dedup_unit`：独立编译运行 `test_dedup_prefetch_unit.cpp`。
- `make test_dedup_integration`：依赖主目标 `iommu_model`，调用 `test_integration_dedup_prefetch.sh` 执行集成测试。
- `test_dedup_prefetch.sh`：运行仿真并通过 `grep` 解析日志验证 PT Cache 命中/MISS、占位符 HIT、PTW 执行次数、Buffer 分配、预取组完成、批量更新、链表刷新等行为。

### 3.6 运行与报告
- `run_report.sh`：设置 `timeout 120` 限制运行时间，通过 `tee` 保存完整日志，并用 `grep -E` 过滤关键统计行（IOMMU STAT、Completed、IOPS、Sim Time、Steady、Efficiency、slave_/master_、Peak、Global、PTW:、DDR、xDTW、Collector、Output、Phase、responses、e2e、Bandwidth、Avg、Max task、Min task、DDR access、DDR lat、task lat、Update、PTWC、NONE、total、window、peak、util）。
- 分析脚本位于 `tmp/` 目录（`analyze_exit_iops_vN.py`、`analyze_pipeline_stages.py`、`calc_iops.sh`、`calc_stats.sh`、`concurrency_analysis.sh` 等），用于生成 CSV/PNG 图表。

## 4. 约束与约定

- **SystemC 安装位置固定**：默认假设 SystemC 安装在 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`，跨平台需修改 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB`。
- **多定义容忍**：链接时强制使用 `--allow-multiple-definition`，表明部分符号可能重复定义（常见于 SystemC 仿真模型的多实例）。
- **调试宏集中管理**：Debug 模式下自动开启一组 `DEBUG_*` 宏，Release 模式关闭全部调试输出。
- **测试场景通过编译期宏配置**：所有仿真参数（带宽、并发度、预取深度、是否跳过 Phase1、虚拟化队列深度等）均通过 `TEST_FLAGS` 中的 `-DTEST_CFG_*` 宏传入，而非运行时参数。
- **WSL 双端构建**：提供 `build_wsl.sh` 将工程拷贝到 WSL 本地文件系统编译后再拷回 Windows，避免跨文件系统性能问题。
- **无容器化/CI**：仓库中未发现 Dockerfile、GitHub Actions、Jenkins 等 CI/CD 配置，构建完全依赖本地 Makefile 和 Shell 脚本。
- **版本管理**：仓库根目录存在多个 `.md` 分析报告文件（如 `BATCH_UPDATE_MISS_ROOT_CAUSE_20260609.md`、`PT_CACHE_DEEP_ANALYSIS_3QUESTIONS_20260609.md`），但未见统一的版本号或发布标签机制。