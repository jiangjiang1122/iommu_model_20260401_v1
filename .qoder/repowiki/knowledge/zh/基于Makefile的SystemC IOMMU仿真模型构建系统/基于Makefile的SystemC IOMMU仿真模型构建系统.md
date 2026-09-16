---
kind: build_system
name: 基于Makefile的SystemC IOMMU仿真模型构建系统
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
    - test_simple.sh
---

## 1. 构建系统与工具链

该项目使用 **GNU Make + g++ (C++17)** 作为核心构建系统，依赖 **SystemC** 库（`-lsystemc -lpthread -lm`）进行周期精确仿真。所有编译/链接参数集中在根目录 `Makefile` 中，辅以多个 `.sh` 脚本提供跨平台与场景化构建入口。

- **编译器**: `g++` / `gcc`，默认 `-std=c++17 -w`，启用 SystemC 动态进程 (`-DSC_INCLUDE_DYNAMIC_PROCESSES`) 并关闭 API 版本检查 (`-DSC_DISABLE_API_VERSION_CHECK`)。
- **SystemC 路径**: 硬编码为 `/usr/include`、`/usr/lib/x86_64-linux-gnu`，通过 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量集中管理。
- **调试/发布模式**: `DEBUG=1`（默认）开启 `-g -O0` 及大量 `DEBUG_*` 宏；`DEBUG=0` 切换为 `-O3 -DNDEBUG`。
- **输出产物**: 可执行文件 `iommu_model`，对象文件统一输出到 `build/` 目录，按源文件相对路径镜像组织（如 `build/iommu/cache_src/cache/pt_cache.o`）。

## 2. 关键构建文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 主构建规则，定义全部源码列表、测试场景选择、编译/链接选项 |
| `main.cpp` | SystemC 仿真入口 `sc_main`，实例化 IOMMU、RP、PCIENOC、SLINK、DDR 模块并绑定 TLM 端口 |
| `build_cpp.sh` | 独立编译脚本，遍历 `cache_src` 下 `.cpp` 逐个编译后链接 |
| `compile_and_test.sh` | 增量编译 `pt_cache.cpp` + `iommu_top.cc` 并运行 |
| `link_and_run.sh` | 仅链接已有 `.o` 并运行仿真 |
| `build_wsl.sh` | WSL 专用：将工程复制到 `/tmp/iommu_build` 本地文件系统编译，再拷回 Windows 目录 |
| `test_simple.sh` | 参数扫描测试：修改 `iommu_perf_params.hh` 中的延迟参数后重新 make 并运行 |
| `build_err.txt` / `build_log.txt` / `build_out.txt` | 历史构建日志（非脚本） |

## 3. 架构与约定

### 3.1 测试场景驱动构建
`Makefile` 通过 `TEST ?= rand4k_singlestage` 变量选择测试线程源文件，每个场景对应 `rp/test_rp_*.cc` 中的一个线程文件，并通过 `TEST_FLAGS` 注入编译期配置宏（如 `-DTEST_TWO_STAGE`、`-DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=5`、`-DTEST_CFG_AXI_PORT_WIDTH_BIT=1024`）。当前已支持的场景包括：
- `rand4k_singlestage`（默认）、`seq128k_singlestage`、`sv48_bare`
- `seq128k_twostage`、`seq128k_twostage_s2on`、`seq128k_twostage_s2on_128g`
- `rand4k_twostage`、`rand4k_twostage_s2on_128g`、`rand4k_twostage_s2on_128g_inval`
- `seq512b_2mb_twostage_s2on`、`seq512b_2mb_twostage_s2on_128g`
- `virt_lazy_twostage`、`virt_strict_twostage`（虚拟化两级 Stage）
- `rand4k_msi_mix_s2on_128g`、`rand4k_msi_mix_s2on_128g_512mb`（MSI混合负载）
- `msi_perf`（MSI地址翻译性能验证）
- `cache_inval`（缓存失效功能测试）

### 3.2 源码组织与编译单元
源码按功能分层组织，`Makefile` 中显式列出所有参与链接的 `.cc/.cpp` 文件：
- `iommu/iommu_fun_model/` — 功能模型实现
- `iommu/iommu_perf_model/` — 性能模型实现
- `iommu/cache_src/{cache,common,replacement,subsystem}/` — 多级缓存子系统
- `rp/` — Root Port 测试驱动（多场景）
- `pcienoc/`、`slink/`、`ddr/` — 外设/NoC/内存模型

所有对象文件通过 `$(patsubst %.cc,build/%.o,...)` 映射到 `build/` 下镜像目录结构。

### 3.3 链接策略
链接时强制使用 `-Wl,--allow-multiple-definition` 以容忍 SystemC 宏展开导致的重复符号；同时使用 `-Wl,--no-as-needed` 确保 `libsystemc` 等库被正确链接。

### 3.4 跨平台/WSL 构建
由于项目位于 Windows 磁盘 (`/mnt/d/...`)，直接编译性能较差，因此提供 `build_wsl.sh` 将工程拷贝至 WSL 原生文件系统 (`/tmp/iommu_build`) 编译后再拷回可执行文件。另有 `compile_wsl.sh`、`run_wsl.sh` 等辅助脚本。

## 4. 约定与约束

- **构建入口约定**: 新增测试场景需在 `Makefile` 的 `ifeq ($(TEST), ...)` 分支中添加对应的 `TEST_THREAD_SRC` 和 `TEST_FLAGS`，并在 `CXX_SOURCES` 中引用该源文件。
- **参数传递约定**: 所有运行时可调参数通过编译期宏 `-DTEST_CFG_*` 注入（如 `TEST_CFG_PTW_MAX_OUTSTANDING_TASKS`、`TEST_CFG_AXI_PORT_WIDTH_BIT`），而非命令行参数。
- **调试宏约定**: 启用 `DEBUG=1` 时会批量定义 `DEBUG_TRANSLATION`、`DEBUG_TWOSTAGE`、`DEBUG_MSITRANS`、`DEBUG_COMMANDS`、`DEBUG_SECONDSTAGE`、`DEBUG_ATC`、`DEBUG_FAULTS`、`DEBUG_INTERRUPT`、`DEBUG_HPM`、`DEBUG_UTILS` 等宏，用于条件编译日志输出。
- **SystemC 集成约定**: 必须包含 `-DSC_INCLUDE_DYNAMIC_PROCESSES` 和 `-DSC_DISABLE_API_VERSION_CHECK`，且需链接 `libsystemc`、`pthread`、`m`。
- **清理约定**: `make clean` 删除 `build/*.o` 和 `$(TARGET)`；`rebuild` 先 clean 再 all。
- **单元测试约定**: PT Cache 去重+预取单元测试通过 `make test_dedup_unit` 单独编译 `test_dedup_prefetch_unit.cpp` 并运行；集成测试通过 `make test_dedup_integration` 调用 `test_integration_dedup_prefetch.sh`。
- **无 Docker/CI**: 仓库未包含 Dockerfile、GitHub Actions 或其他 CI 配置文件，构建完全依赖本地 Makefile 与 shell 脚本。
- **无版本化发布流程**: 没有独立的版本号管理或发布脚本，可执行文件直接命名为 `iommu_model`。

## 5. 适用性说明

本仓库是一个 C++ SystemC 仿真模型项目，构建系统由 Makefile 与若干 shell 脚本组成，属于典型的学术/原型级构建体系，不具备企业级 CI/CD、容器化或交叉编译流水线特征。