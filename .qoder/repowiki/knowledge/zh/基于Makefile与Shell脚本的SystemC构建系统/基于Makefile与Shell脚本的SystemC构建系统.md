---
kind: build_system
name: 基于Makefile与Shell脚本的SystemC构建系统
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
---

## 1. 使用的系统与工具
- **构建系统**: GNU Make（`Makefile`）作为核心构建编排，配合多个 Bash Shell 脚本完成跨平台/WSL 编译、链接与运行。
- **编译器**: g++ (C++17) + gcc，依赖 SystemC 库（`-lsystemc`），并链接 `pthread`、`m`。
- **仿真框架**: SystemC/TLM2.0，通过 `-DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK` 启用动态进程与版本检查关闭。
- **输出产物**: 单一可执行文件 `iommu_model`，对象文件统一输出到 `build/` 目录。

## 2. 关键文件与包
- **顶层构建入口**: `Makefile`、`main.cpp`（`sc_main` 组装 IOMMU/RP/PCIENOC/SLINK/DDR 模块并绑定 TLM socket）
- **辅助构建脚本**:
  - `build_cpp.sh`：逐个编译 cache_src 下的 .cpp 并链接
  - `compile_and_test.sh`：增量编译 pt_cache/iommu_top 后链接运行
  - `link_and_run.sh`：仅链接已存在的 .o 并运行
  - `build_wsl.sh`：在 WSL 本地文件系统编译，再拷贝回 Windows 路径
- **测试场景源文件**: `rp/test_rp_*.cc`、`pcienoc/test_pcienoc.cc`、`slink/test_slink.cc`、`ddr/test_ddr.cc`
- **单元测试**: `test_dedup_prefetch_unit.cpp`（独立编译为 `test_dedup_unit`）

## 3. 架构与约定
- **多场景测试驱动**: `Makefile` 通过 `TEST=...` 变量选择不同测试线程源文件（如 `rand4k_singlestage`、`seq128k_twostage`、`virt_lazy_twostage` 等），并为每个场景注入对应的 `-DTEST_*` 宏参数，实现“一个工程、多种负载”的构建复用。
- **模块化源码组织**: 源码按功能分层——`iommu/iommu_fun_model`（功能模型）、`iommu/iommu_perf_model`（性能模型）、`iommu/cache_src/{cache,common,replacement,subsystem}`（缓存子系统）、`rp/slink/pcienoc/ddr`（外设/总线/内存模型）。`Makefile` 显式列举所有 `.cc/.cpp` 参与编译。
- **对象文件隔离**: 所有 `.o` 输出到 `build/` 下，保持源码树干净；`clean` 目标删除整个 `build/` 目录。
- **调试/发布模式**: `DEBUG=1`（默认）开启 `-g -O0` 及大量 `DEBUG_*` 宏；`DEBUG=0` 切换为 `-O3 -DNDEBUG` 发布优化。
- **SystemC 集成**: `main.cpp` 中 `sc_main` 负责实例化各模块、绑定 AXI/TLM socket，并通过 `sc_start(300000000, SC_NS)` 启动 300ms 仿真。

## 4. 约定与约束
- **编译器与标准**: 强制使用 C++17（`-std=c++17`），禁止警告（`-w`），包含路径固定指向 `/usr/include` 与项目内 `./iommu`、`./iommu/cache_src` 等子目录。
- **SystemC 安装位置**: 默认假设 SystemC 安装在 `/usr`（include 在 `/usr/include`，lib 在 `/usr/lib/x86_64-linux-gnu`），可通过修改 `SYSTEMC_PREFIX`/`SYSTEMC_INCLUDE`/`SYSTEMC_LIB` 调整。
- **链接选项**: 始终使用 `-Wl,--no-as-needed -Wl,--allow-multiple-definition` 以兼容 SystemC 的多定义符号行为。
- **测试场景命名规范**: 测试线程文件遵循 `test_rp_<pattern>_<stage>_thread.cc` 命名约定，`Makefile` 中的 `ifeq ($(TEST), ...)` 分支严格匹配这些名称。
- **WSL 双环境支持**: `build_wsl.sh` 约定将工程复制到 `/tmp/iommu_build` 进行原生 Linux 编译，再将 `iommu_model` 拷回 Windows 路径，避免跨文件系统性能问题。
- **无容器化/CI**: 仓库未发现 Dockerfile、GitHub Actions、Jenkins 等 CI/CD 配置，构建完全依赖本地 Make + Shell 脚本。
- **依赖声明方式**: 未使用 `pkg-config` 或外部包管理器，SystemC 依赖通过直接 `-I` 和 `-L` 路径硬编码管理。

## 5. 构建流程概览
```
make TEST=<scenario> DEBUG=<0|1>
  → 选择测试源文件 + 注入宏参数
  → 编译所有 .cc/.cpp → build/*.o
  → 链接 iommu_model（依赖 systemc/pthread/m）
  → 运行 ./iommu_model 执行 SystemC 仿真
```

### 典型命令示例
- `make rand4k_singlestage`：默认单级随机读测试
- `make seq128k_twostage`：两阶段顺序读测试
- `make virt_lazy_twostage VIRT_FQ_DEPTH=32`：虚拟化 Lazy 模式失效测试
- `make test_dedup_unit`：独立编译运行 PT Cache 去重单元测试
- `./build_wsl.sh seq128k_twostage`：WSL 环境下完整构建流程
