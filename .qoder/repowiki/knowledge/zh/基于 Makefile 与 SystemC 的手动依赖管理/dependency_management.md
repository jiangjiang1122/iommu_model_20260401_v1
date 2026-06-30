该项目采用传统的 **Makefile** 结合 **GNU Make** 进行构建和依赖管理，主要依赖外部系统级库 **SystemC**。项目中不存在现代化的包管理器（如 `npm`, `pip`, `cargo` 等）或依赖清单文件（如 `go.mod`, `package.json`）。

### 1. 依赖声明与获取
- **外部依赖**：核心依赖为 **SystemC** 库。在 `Makefile` 中通过 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB` 变量硬编码指定路径（默认为 `/usr` 和 `/usr/lib/x86_64-linux-gnu`）。
- **内部模块**：项目由多个子模块组成（`iommu/`, `rp/`, `pcienoc/`, `slink/`, `ddr/`），所有源文件（`.cc`, `.cpp`）在 `Makefile` 的 `CXX_SOURCES` 变量中显式列出。
- **无自动解析**：没有依赖自动下载或解析机制。开发者需手动确保 SystemC 库已安装在指定路径，或通过修改 `Makefile`/环境变量指向正确的安装位置。

### 2. 构建与链接规则
- **编译流程**：使用 `g++` (C++17 标准) 编译所有源文件至 `build/` 目录下的对象文件（`.o`）。
- **链接依赖**：最终可执行文件 `iommu_model` 链接时依赖 `-lsystemc`、`-lpthread` 和 `-lm`。
- **条件编译**：通过 `TEST` 变量选择不同的测试线程源文件（如 `test_rp_rand4k_single_stage_thread.cc`），并通过 `TEST_FLAGS` 注入宏定义（如 `-DTEST_TWO_STAGE`）来配置仿真行为。

### 3. 版本管理与锁定
- **无锁文件**：项目中不存在 `lock` 文件（如 `package-lock.json`, `go.sum`）。依赖版本由开发环境中的 SystemC 安装版本决定（README 建议 SystemC 2.3.x）。
- **环境耦合**：构建脚本（`compile_wsl.sh`, `build_cpp.sh`）强依赖于 WSL (Windows Subsystem for Linux) 环境及特定的文件系统路径（如 `/mnt/d/...`），表明依赖管理高度耦合于本地开发环境配置。

### 4. 开发者规范
- **环境准备**：开发者必须在 WSL/Linux 环境中手动安装 SystemC 库，并确保头文件和库文件位于 `Makefile` 指定的路径，或手动修改 `Makefile` 中的 `SYSTEMC_*` 变量。
- **构建命令**：统一使用 `make clean && make all` 或提供的 shell 脚本进行构建，避免直接调用编译器导致依赖遗漏。
- **依赖更新**：若需更新 SystemC 版本，需手动替换系统库并重新编译整个项目，无自动化升级工具支持。