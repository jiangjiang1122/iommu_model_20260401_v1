## 1. 构建系统概览
该项目采用 **GNU Make** 作为核心构建工具，配合一系列 **Shell/Batch 脚本** 实现跨环境（Linux/WSL/Windows）的编译、链接与测试。项目基于 **SystemC/TLM-2.0** 标准，通过 `g++` (C++17) 进行编译。

### 核心特征：
- **手动依赖管理**：未使用 CMake/Autotools，而是通过 `Makefile` 显式列出所有源文件并硬编码 SystemC 库路径。
- **多场景测试驱动**：通过 `TEST` 变量在编译时动态切换测试线程（如随机访问、顺序访问、Sv48模式）。
- **分层调试支持**：提供 Debug/Release 两种构建模式，并通过宏定义控制细粒度的模块级日志输出。

## 2. 关键构建文件与逻辑

### 2.1 Makefile (核心构建入口)
- **编译器配置**：默认使用 `g++`，强制开启 `-std=c++17`。
- **SystemC 集成**：硬编码指向 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`。若环境不同需手动修改 `SYSTEMC_PREFIX`。
- **源文件组织**：将 `iommu/` 下的功能模型、性能模型、缓存子系统以及外围模拟模块（DDR, PCIeNoC, SLINK）统一编译为对象文件，存放于 `build/` 目录。
- **测试场景选择**：
  - `make TEST=rand4k` (默认): 4KB 随机读压力测试。
  - `make TEST=seq128k`: 128KB 顺序读测试。
  - `make TEST=sv48_bare`: Sv48 页表格式兼容性测试。
- **单元测试目标**：
  - `test_dedup_unit`: 独立编译 `test_dedup_prefetch_unit.cpp`，不依赖 SystemC 库，用于快速验证去重算法逻辑。
  - `test_dedup_integration`: 运行全系统仿真并通过 Shell 脚本分析日志输出。

### 2.2 辅助脚本
- **跨平台支持**：
  - `compile_systemc.bat`: Windows 环境下使用 CMake 编译 SystemC 源码并设置环境变量。
  - `compile_wsl.sh` / `run_wsl.sh`: 针对 WSL (Windows Subsystem for Linux) 环境的封装脚本，处理路径映射 (`/mnt/d/...`)。
- **自动化分析**：根目录下存在大量 `analyze_*.sh` 和 `analyze_*.py` 脚本，用于在仿真结束后自动提取 PT Cache 命中率、DDR 延迟等性能指标。

## 3. 架构约定与开发规范

### 3.1 目录结构与构建产物
- **源码隔离**：核心逻辑位于 `iommu/`，外围模拟组件位于 `ddr/`, `pcienoc/`, `slink/`, `rp/`。
- **构建产物**：所有 `.o` 文件必须生成在 `build/` 目录下，保持源码树整洁。执行文件名为 `iommu_model`。

### 3.2 调试与日志规范
- **宏控制日志**：通过 `-DDEBUG_TRANSLATION`, `-DDEBUG_FAULTS` 等宏在编译期开启特定模块的详细追踪。生产环境应使用 `DEBUG=0 make` 关闭这些宏以提升性能。
- **SystemC 动态进程**：编译标志中固定包含 `-DSC_INCLUDE_DYNAMIC_PROCESSES`，表明模型大量使用了 `sc_spawn` 等动态并发机制。

### 3.3 测试与验证流程
1. **单元测试**：优先运行 `make test_dedup_unit` 验证核心算法（如去重 Buffer 链表操作）的正确性。
2. **集成仿真**：运行 `make` 生成全系统模型，通过 `main.cpp` 启动 SystemC 内核。
3. **结果审计**：利用 `test_integration_dedup_prefetch.sh` 等脚本对仿真日志进行正则匹配，自动判定预取组初始化、占位符命中、批量更新等关键行为是否符合预期。

## 4. 开发者注意事项
- **环境适配**：若 SystemC 非系统默认安装，必须修改 `Makefile` 中的 `SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB` 变量。
- **链接选项**：使用了 `-Wl,--allow-multiple-definition` 以解决 SystemC 某些头文件内联函数可能引发的符号重复问题。
- **清理构建**：执行 `make clean` 会彻底删除 `build/` 目录，重新构建时需确保目录权限正常。