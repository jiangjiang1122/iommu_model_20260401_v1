该项目是一个基于 SystemC/TLM-2.0 的 RISC-V IOMMU 性能仿真模型，其依赖管理主要围绕 **SystemC 库** 的手动集成与构建环境配置展开。由于 SystemC 并非通过现代包管理器（如 vcpkg, conan）自动获取，而是依赖于开发者在本地环境（WSL/Linux 或 Windows）中手动编译安装，因此其“依赖管理”体现为构建脚本中的路径硬编码与环境变量约定。

### 1. 核心依赖：SystemC 库
- **版本要求**：SystemC 2.3.x（参考 `README.md` 及 `compile_systemc.bat` 中的 `systemc-2.3.3`）。
- **获取方式**：
  - **Linux/WSL**：通常通过系统包管理器安装（如 `apt-get install libsystemc-dev`），或通过源码编译后安装至 `/usr` 目录。
  - **Windows**：需从 Accellera 官网下载源码，使用 CMake 进行编译和安装（见 `compile_systemc.bat`）。

### 2. 构建系统与路径配置
项目使用 **GNU Make** 作为主要构建工具，并辅以多个 Shell/Batch 脚本以适配不同环境。

- **Makefile**：
  - 定义了 SystemC 的头文件与库文件路径：
    - `SYSTEMC_INCLUDE = /usr/include`
    - `SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu`
  - 链接标志：`-lsystemc -lpthread -lm`。
  - 编译器标准：`-std=c++17`。
  - 包含大量内部模块头文件路径（如 `iommu/include`, `iommu/cache_src` 等）。

- **辅助脚本**：
  - `compile_wsl.sh`：专为 WSL 环境设计，调用 `make clean && make all`。
  - `build_cpp.sh`：手动指定编译标志和对象文件，用于更细粒度的构建控制。
  - `compile_systemc.bat`：Windows 环境下用于从源码编译 SystemC 库本身的脚本，设置了 `SYSTEMC_HOME` 等环境变量。

### 3. 第三方组件
- **JSON 解析**：项目内部包含了一个轻量级的 JSON 解析实现（`iommu/cache_src/common/mini_json.h`, `json_config.cpp`），未依赖外部 JSON 库（如 nlohmann/json 或 rapidjson），体现了“零外部依赖”的设计倾向。
- **TLM-2.0**：作为 SystemC 的一部分，用于建模 PCIe NoC、DDR 等接口的通信协议（见 `ddr/test_ddr.cc` 中的 `tlm::tlm_sync_enum`）。

### 4. 开发者约束与惯例
- **环境隔离**：强烈建议在 **WSL2 (Ubuntu)** 环境中进行编译和运行，以避免 Windows 原生编译带来的兼容性问题。
- **路径硬编码风险**：`Makefile` 和脚本中硬编码了 `/usr` 或 `D:\TOOLS\systemc-2.3.3` 等路径。若开发者的 SystemC 安装位置不同，需手动修改这些文件。
- **无锁文件/包管理器**：项目未使用 `package.json`, `go.mod`, `Cargo.toml` 等现代依赖管理工具，也没有 `vendor` 目录。所有第三方依赖（仅 SystemC）均视为“系统级依赖”，由开发者自行确保环境一致性。