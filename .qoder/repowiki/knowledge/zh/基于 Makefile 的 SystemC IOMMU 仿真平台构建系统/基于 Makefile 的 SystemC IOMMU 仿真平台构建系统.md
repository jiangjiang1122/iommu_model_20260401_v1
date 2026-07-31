---
kind: build_system
name: 基于 Makefile 的 SystemC IOMMU 仿真平台构建系统
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

该仓库使用纯 Makefile 作为核心构建系统，配合多个 Bash 脚本完成编译、链接、测试与跨环境（WSL/Windows）构建流程。整体采用 g++ C++17 + SystemC/TLM-2.0 工具链，输出单一可执行文件 `iommu_model`。

**构建工具与依赖**
- 编译器：g++ (C++17)，gcc；SystemC 库通过 `-lsystemc -lpthread -lm` 链接，默认路径 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`
- 构建系统：GNU Make；无 CMake/Bazel/Ninja 等高级构建工具参与项目自身构建（仅 `compile_systemc.bat` 用 cmake 编译 SystemC 源码本身）
- 目标产物：`build/*.o` 中间对象文件 + 根目录 `iommu_model` 可执行文件

**核心构建规则**
- `Makefile` 是主入口，通过 `TEST` 变量选择不同测试场景（如 `rand4k_singlestage`、`seq128k_twostage`、`sv48_bare`、`seq512b_2mb_twostage_s2on` 等），每个场景对应 `rp/` 下独立的线程源文件，并通过 `TEST_FLAGS` 注入编译期宏配置（如 `TEST_TWO_STAGE`、`TEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3`、`TEST_CFG_WALKER_CACHE_S2_ENABLED=1` 等）
- 所有 `.cc`/`.cpp` 源文件统一编译到 `build/` 目录，保持源码树干净
- Debug/Release 模式由 `DEBUG` 变量控制：Debug 开启 `-g -O0` 及大量 `DEBUG_*` 宏；Release 使用 `-O3 -DNDEBUG`
- 链接时强制 `-Wl,--allow-multiple-definition` 以容忍 SystemC 动态进程中的多定义问题

**辅助构建脚本**
- `build_cpp.sh`：直接遍历 `cache_src/` 下的 .cpp 文件逐个编译并链接，用于快速验证 cache 子系统改动
- `compile_and_test.sh` / `link_and_run.sh`：分步编译关键模块后链接运行，便于调试特定文件
- `build_wsl.sh` / `compile_wsl.sh`：在 WSL 本地文件系统编译后将二进制拷回 Windows 目录，解决跨文件系统性能问题
- `test_dedup_unit` 与 `test_dedup_integration`：Makefile 内置目标，分别编译独立单元测试 `test_dedup_prefetch_unit.cpp` 和运行集成测试脚本

**顶层入口与模块绑定**
- `main.cpp` 通过 `sc_main` 创建 `iommu_top`、`DDR_Module`、`RP_Module`、`PCIENOC_Module`、`SLINK_Module` 五个 SystemC 模块实例，并使用 TLM socket 进行互连绑定，随后调用 `sc_start(300000000, SC_NS)` 启动仿真

**约束与约定**
- 所有新增源文件需手动加入 `Makefile` 的 `CXX_SOURCES` 列表，否则不会被纳入构建
- 测试场景必须遵循 `rp/test_rp_*.cc` 命名规范，并在 `Makefile` 的 `ifeq ($(TEST), ...)` 分支中注册对应的 `TEST_THREAD_SRC` 与 `TEST_FLAGS`
- SystemC 安装路径固定为 `/usr`，若安装在其他位置需同步修改 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 三个变量
- 未提供 Dockerfile、CI 流水线或交叉编译配置，构建完全依赖本地 Linux/WSL 环境