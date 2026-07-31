---
kind: build_system
name: SystemC IOMMU 性能模型构建系统
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

该项目使用基于 GNU Make 的 C++/SystemC 构建系统，配合多个 Bash 脚本完成编译、链接、测试和跨环境部署。核心构建流程如下：

**1. 主要构建工具与依赖**
- 编译器：g++ (C++17)，gcc
- 仿真框架：SystemC（通过 `-lsystemc` 链接），启用 `SC_INCLUDE_DYNAMIC_PROCESSES` 和 `SC_DISABLE_API_VERSION_CHECK`
- 链接选项：`--no-as-needed`、`--allow-multiple-definition`、`-Wl,--no-as-needed -lpthread -lm`
- SystemC 路径硬编码为 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`

**2. 核心构建文件**
- `Makefile`：主构建入口，定义所有源文件列表、测试场景、编译标志和目标
- `main.cpp`：SystemC 仿真入口，实例化 iommu_top、DDR、RP、PCIENOC、SLINK 模块并绑定 TLM 套接字
- `build_cpp.sh`：增量编译 cache_src 下的 .cpp 文件并链接
- `compile_and_test.sh` / `link_and_run.sh`：单文件编译与快速链接运行脚本
- `build_wsl.sh`：WSL 本地文件系统编译后拷回 Windows 的跨平台脚本

**3. 测试场景选择机制**
Makefile 通过 `TEST` 变量选择不同测试线程源文件和编译宏：
- `rand4k_singlestage`（默认）：4KB随机读 + 单级翻译
- `seq128k_singlestage`：128KB顺序读 + 单级翻译
- `sv48_bare`：Sv48 + Bare 基础测试
- `seq128k_twostage` / `rand4k_twostage`：两阶段地址翻译
- 支持 S2 开启、128GB/s 端口宽度、全局并发 512、PTW 并发等参数通过 `-DTEST_CFG_*` 宏配置

**4. 构建产物与目录结构**
- 目标可执行文件：`iommu_model`
- 对象文件输出到 `build/` 目录，保持源码树整洁
- 清理目标：`make clean` 删除 `build/*.o` 和 `iommu_model`

**5. 调试与优化模式**
- `DEBUG=1`（默认）：启用 `-g -O0` 及大量 `DEBUG_*` 宏（DEBUG_TRANSLATION、DEBUG_TWOSTAGE、DEBUG_MSITRANS 等）
- `DEBUG=0`：Release 模式，`-O3 -DNDEBUG`
- WSL 构建脚本默认使用 `DEBUG=0` 以获得更好性能

**6. 单元测试与集成测试**
- `test_dedup_unit`：独立编译 `test_dedup_prefetch_unit.cpp` 生成 `test_dedup_unit` 可执行文件
- `test_dedup_integration`：调用 `test_integration_dedup_prefetch.sh` 进行集成测试
- `tmp/` 目录下包含大量分析脚本（`.sh`、`.py`、`.ps1`），用于性能分析和回归测试

**7. 约束与约定**
- 所有 SystemC 模块必须通过 TLM 套接字通信（simple_initiator_socket/simple_target_socket）
- 源文件按功能分层：`iommu/iommu_fun_model/`（功能模型）、`iommu/iommu_perf_model/`（性能模型）、`iommu/cache_src/`（缓存子系统）、`rp/`（请求生成器）、`pcienoc/`、`slink/`、`ddr/`
- 构建时强制使用 C++17 标准，禁用警告（`-w`）以兼容第三方代码