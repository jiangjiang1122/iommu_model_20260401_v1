---
kind: dependency_management
name: SystemC 依赖管理与构建配置
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - compile_systemc.bat
    - iommu/cache_src/common/mini_json.h
---

该项目是一个基于 SystemC/TLM-2.0 的 RISC-V IOMMU 性能仿真模型，其依赖管理主要围绕 **SystemC 库** 的手动集成与构建环境配置展开。由于 C++/SystemC 生态缺乏统一的包管理器（如 npm 或 cargo），本项目采用传统的 **Makefile + 系统级库链接** 方式管理核心依赖。

### 1. 核心依赖：SystemC/TLM-2.0
- **依赖来源**：SystemC 并非通过包管理器自动获取，而是要求开发者在宿主机上手动编译安装或通过系统包管理器（如 Linux 下的 `apt`）安装。
- **版本约束**：代码中使用了 `sc_module`, `tlm::tlm_phase`, `sc_time` 等标准接口，并定义了 `SC_DISABLE_API_VERSION_CHECK` 以跳过严格的 API 版本校验，暗示其对 SystemC 2.3.x 系列具有兼容性要求。
- **Windows 环境支持**：提供了 `compile_systemc.bat` 脚本，用于在 Windows 环境下从源码编译 SystemC 2.3.3，并设置 `SYSTEMC_HOME`、`SYSTEMC_INCLUDE` 和 `SYSTEMC_LIBDIR` 环境变量。

### 2. 构建系统与链接配置
- **Makefile 驱动**：根目录下的 `Makefile` 是主要的构建入口。它硬编码了 Linux 环境下的默认路径（`/usr/include` 和 `/usr/lib/x86_64-linux-gnu`）。
- **编译器标志**：
  - `-std=c++17`：强制使用 C++17 标准。
  - `-DSC_INCLUDE_DYNAMIC_PROCESSES`：启用 SystemC 动态进程支持。
  - `-lsystemc`：在链接阶段显式链接 SystemC 库。
  - `-Wl,--allow-multiple-definition`：处理可能的符号重复定义问题，常见于大型 SystemC 模块链接。
- **辅助脚本**：`build_cpp.sh` 和 `compile_and_test.sh` 提供了更细粒度的编译控制，允许针对特定模块（如 `pt_cache.cpp`）进行独立编译和测试。

### 3. 第三方工具链依赖
- **JSON 处理**：项目内部实现了轻量级的 JSON 解析器（`iommu/cache_src/common/mini_json.h`），未引入外部 JSON 库（如 nlohmann/json），体现了“零外部依赖”的设计倾向。
- **Python 分析工具**：`tmp/` 和根目录下的多个 `.py` 脚本（如 `analyze_pt_cache_access.py`）依赖 Python 环境及基础数据科学库（如 `matplotlib`, `numpy`），但这些依赖未在项目中显式声明（无 `requirements.txt`）。

### 4. 开发者约定
- **环境准备**：开发者需确保系统中已安装 SystemC 库。在 Linux 上通常通过 `sudo apt install libsystemc-dev` 完成；在 Windows 上需运行 `compile_systemc.bat` 预先构建。
- **路径适配**：若 SystemC 安装在非标准路径，需手动修改 `Makefile` 中的 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB` 变量。
- **单元测试**：提供了独立的单元测试目标 `test_dedup_unit`，该目标不依赖 SystemC 库，直接使用 `g++` 编译纯 C++ 逻辑，便于快速验证算法正确性。