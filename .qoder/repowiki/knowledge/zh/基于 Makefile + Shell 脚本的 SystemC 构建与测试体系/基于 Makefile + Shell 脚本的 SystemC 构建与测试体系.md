---
kind: build_system
name: 基于 Makefile + Shell 脚本的 SystemC 构建与测试体系
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
---

该仓库使用纯 Makefile 驱动的 C++/SystemC 构建系统，辅以多个 Shell 脚本完成编译、链接、运行与测试。整体风格为本地开发导向，未集成 CI/CD 或容器化流程。

**1. 构建工具与依赖**
- 编译器：g++ (C++17)，gcc；依赖 SystemC 库（`-lsystemc`）、pthread、math 库。
- SystemC 路径硬编码为 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`，通过 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量可调。
- 仿真入口为 `main.cpp`，调用 `sc_main` 搭建 IOMMU、DDR、RP、PCIENOC、SLINK 等模块并绑定 TLM 端口。

**2. 核心构建文件**
- `Makefile`：主构建入口，定义所有源文件列表、目标、编译选项、调试宏、测试场景选择。
- `build_cpp.sh`：按顺序编译 cache_src 下的 .cpp 文件并链接生成 `iommu_model` 可执行文件。
- `compile_and_test.sh` / `link_and_run.sh`：增量编译特定源文件后链接运行，用于快速验证修改。
- `test_*.sh`、`check_*.sh`、`extract_*.sh`、`run_*.sh`：大量场景化测试与分析脚本，覆盖单测、集成测试、回归、性能分析等。
- `build/*.o`：对象文件输出目录，由 Makefile 规则自动创建。

**3. 构建架构与约定**
- **源码组织**：IOMMU 功能模型 (`iommu/iommu_fun_model`)、性能模型 (`iommu/iommu_perf_model`)、缓存子系统 (`iommu/cache_src`)、测试线程 (`rp/test_rp_*.cc`)、外设模型 (`pcienoc`, `slink`, `ddr`) 分别位于独立子目录，统一由 `CXX_SOURCES` 变量收集。
- **测试场景驱动**：通过 `make TEST=<scenario>` 选择不同测试线程（如 `rand4k_singlestage`、`seq128k_twostage`、`virt_lazy_twostage` 等），每个场景对应一组预定义的 `TEST_FLAGS` 编译宏，控制行为参数（带宽、并发、预取深度、虚拟化模式等）。
- **调试/发布模式**：`DEBUG=1`（默认）启用 `-g -O0` 及大量 `DEBUG_*` 宏；`DEBUG=0` 启用 `-O3 -DNDEBUG`。
- **对象文件隔离**：所有 `.o` 文件输出到 `build/` 目录，保持源码树干净。
- **链接选项**：强制 `--allow-multiple-definition` 以容忍 SystemC/TLM 中的多定义问题。

**4. 测试与验证流程**
- **单元测试**：`make test_dedup_unit` 单独编译运行 `test_dedup_prefetch_unit.cpp`。
- **集成测试**：`make test_dedup_integration` 调用 `test_integration_dedup_prefetch.sh` 运行端到端验证。
- **场景化测试**：通过 `TEST=` 变量切换不同负载（随机/顺序、单级/两级翻译、虚拟化 Lazy/Strict 失效等），每个场景有专用线程文件。
- **辅助分析**：`tmp/` 目录下大量 Python 脚本用于分析 PTW 延迟、DDI/O 间隔、命中率统计等。

**5. 约束与限制**
- 无 Dockerfile、CI 配置、跨平台支持（脚本中硬编码 Linux 路径 `/mnt/d/...` 和 `/usr/lib/x86_64-linux-gnu`）。
- 无版本管理脚本（版本号由文件名和文档手动维护）。
- 构建系统完全依赖 GNU Make 和 Bash，不支持其他构建系统（CMake、Ninja 等）。
- SystemC 必须预先安装到 `/usr` 前缀下，否则需修改 `SYSTEMC_*` 变量。