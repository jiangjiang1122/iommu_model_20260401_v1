---
kind: build_system
name: SystemC/C++ 构建系统（Makefile + Shell 脚本）
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - build_wsl.sh
    - compile_systemc.bat
---

本项目采用基于 GNU Make 的 C++/SystemC 构建系统，辅以多个 Shell 脚本支持不同平台与测试场景。核心特点如下：

1. **构建工具链**：使用 g++ (C++17) 编译 SystemC 模型，依赖 systemc 库、pthread 和 math 库；通过 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量指定 SystemC 安装路径（默认 `/usr`）。

2. **核心构建文件**：
   - `Makefile`：主构建入口，定义所有源文件、目标、编译器标志和测试场景
   - `main.cpp`：SystemC 仿真入口，实例化 IOMMU、DDR、RP、PCIENOC、SLINK 模块并绑定 TLM 端口
   - `build_cpp.sh`：直接编译 cache_src 子系统的便捷脚本
   - `compile_and_test.sh`：增量编译+运行测试脚本

3. **测试场景驱动**：通过 `TEST` 变量选择不同测试场景（rand4k_singlestage、seq128k_twostage、sv48_bare等），每个场景对应独立的线程源文件和编译宏定义。

4. **多平台支持**：
   - `build_wsl.sh`：WSL 环境下的完整构建流程（复制工程→编译→拷回可执行文件）
   - `compile_systemc.bat`：Windows 环境下编译安装 SystemC 的批处理脚本

5. **构建输出**：对象文件统一输出到 `build/` 目录，最终生成 `iommu_model` 可执行文件。

6. **调试支持**：通过 `DEBUG=1` 启用详细调试宏（DEBUG_TRANSLATION、DEBUG_TWOSTAGE 等），默认关闭优化以方便调试。

7. **单元测试**：提供 `test_dedup_unit` 和 `test_dedup_integration` 目标，分别编译独立单元测试和运行集成测试脚本。

8. **依赖管理**：无外部依赖管理工具，直接链接 systemc 库；SystemC 需预先安装或通过 `compile_systemc.bat` 编译安装。

9. **无 CI/CD**：未发现 GitHub Actions、Jenkins、Docker 等自动化构建配置，构建主要依赖本地 Makefile 和 Shell 脚本。