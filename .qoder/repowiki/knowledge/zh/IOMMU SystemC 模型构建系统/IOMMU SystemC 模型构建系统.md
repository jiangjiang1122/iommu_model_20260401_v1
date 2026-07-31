---
kind: build_system
name: IOMMU SystemC 模型构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - build_wsl.sh
    - compile_wsl.sh
    - link_and_run.sh
---

## 构建系统与工具链

该项目使用 **GNU Make + g++ (C++17)** 作为核心构建系统，基于 **SystemC/TLM-2.0** 框架实现 RISC-V IOMMU 的 SystemC 多核仿真模型。

### 核心构建文件
- **Makefile**: 主构建入口，定义编译选项、源文件列表、测试场景和链接规则
- **main.cpp**: SystemC 仿真入口，实例化 iommu_top、DDR、RP、PCIENOC、SLINK 模块并建立 TLM 连接
- **build_cpp.sh**: 增量编译脚本，逐个编译 cache_src 下的 .cpp 文件并链接
- **compile_and_test.sh**: 单文件编译+链接+运行的一体化脚本
- **build_wsl.sh / compile_wsl.sh**: WSL 环境专用构建脚本，支持 Windows/WSL 跨文件系统构建
- **link_and_run.sh**: 仅链接并运行已编译对象的辅助脚本

### 编译器与依赖配置
- **编译器**: g++ (C++17)，gcc
- **SystemC 路径**: `/usr/include` (头文件), `/usr/lib/x86_64-linux-gnu` (库文件)
- **关键宏**: `SC_INCLUDE_DYNAMIC_PROCESSES`, `SC_DISABLE_API_VERSION_CHECK`
- **调试模式**: `-g -O0 -DDEBUG` 启用大量 DEBUG_* 宏（DEBUG_TRANSLATION, DEBUG_TWOSTAGE, DEBUG_MSITRANS 等）
- **优化模式**: `-O3 -DNDEBUG`
- **链接选项**: `-Wl,--allow-multiple-definition` 允许重复定义（SystemC 特性）

### 构建架构设计

**分层源码组织**:
- `iommu/iommu_fun_model/`: IOMMU 功能模型（地址翻译、中断、故障处理等）
- `iommu/iommu_perf_model/`: 性能分析模型（统计收集、解析器、缓存包装器）
- `iommu/cache_src/`: 缓存子系统（DC/PC/PT/Walker/MSIPT 缓存、替换策略、去重缓冲）
- `rp/`, `pcienoc/`, `slink/`, `ddr/`: 外设与总线模型

**测试场景驱动构建**:
通过 `TEST` 变量选择不同测试线程：
- `rand4k_singlestage`: 4KB 随机读 + 单级地址翻译（默认）
- `seq128k_singlestage`: 128KB 顺序读 + 单级翻译
- `seq128k_twostage`: 两阶段 Sv48/Sv48x4 地址翻译
- `sv48_bare`: Sv48 + Bare 基础测试

每个场景对应独立的 `test_rp_*_thread.cc` 文件，并通过 `TEST_FLAGS` 传递编译期配置参数。

### 构建流程
1. **对象文件生成**: 所有 `.cc/.cpp` 文件编译为 `build/<path>.o`，保持源码目录结构
2. **目标文件链接**: 链接所有对象文件生成 `iommu_model` 可执行文件
3. **SystemC 运行时**: 链接 `libsystemc`、`pthread`、`m` 库
4. **清理机制**: `make clean` 删除 `build/` 目录和可执行文件

### 跨平台构建支持
- **WSL 集成**: `build_wsl.sh` 将项目复制到 WSL 本地文件系统 (`/tmp/iommu_build`) 进行构建，避免 Windows/WSL 文件系统性能问题
- **Windows 兼容**: 提供 `compile_systemc.bat` 批处理文件用于 Windows 原生编译
- **路径管理**: 脚本中硬编码 `/mnt/d/Qoder_proj/iommu_model_20260401_v1` 路径，依赖固定项目位置

### 测试与验证
- **单元测试**: `make test_dedup_unit` 编译并运行 PT Cache 去重预取单元测试
- **集成测试**: `make test_dedup_integration` 运行完整的集成测试脚本
- **自定义测试**: 通过 `TEST_FLAGS` 传递编译时配置，如 `TEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3`

### 构建约束与约定
- 所有源文件必须遵循命名规范：`.cc` 用于 C++ 源，`.hh` 用于头文件
- 对象文件必须放置在 `build/` 目录下，保持源码目录层次结构
- SystemC 模块必须在 `sc_main()` 中实例化并通过 TLM socket 绑定
- 调试输出通过 `printf` + `fflush(stdout)` 确保实时输出
- 构建失败时立即退出（`exit 1`），不继续后续步骤