---
kind: build_system
name: 基于Makefile的SystemC构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - build_wsl.sh
    - link_and_run.sh
---

## 构建系统与工具链

该项目使用 **GNU Make + g++** 作为核心构建系统，依赖 **SystemC** 库进行硬件性能仿真建模。构建流程围绕顶层 `Makefile` 组织，辅以多个 Shell 脚本支持不同平台与测试场景。

### 1. 构建工具与依赖
- **编译器**: g++ (C++17标准), gcc
- **仿真框架**: SystemC (`/usr/include`, `/usr/lib/x86_64-linux-gnu`)，通过 `-lsystemc -Wl,--no-as-needed` 链接
- **线程库**: pthread、数学库 math
- **关键宏**: `SC_INCLUDE_DYNAMIC_PROCESSES`、`SC_DISABLE_API_VERSION_CHECK` 用于启用SystemC动态进程和禁用API版本检查

### 2. 构建架构与目标
- **主目标**: `iommu_model` 可执行文件，由 `main.cpp` 入口启动SystemC仿真
- **对象文件输出**: 统一放置在 `build/` 目录下，保持源码目录整洁
- **多场景测试**: 通过 `TEST` 变量选择不同测试场景（rand4k_singlestage、seq128k_twostage、sv48_bare等），每个场景对应独立的测试线程文件和编译参数
- **调试/发布模式**: `DEBUG=1` 启用详细调试信息和-O0优化，`DEBUG=0` 使用-O3优化发布版本

### 3. 源文件组织结构
构建系统按功能模块组织源文件：
- `iommu/iommu_fun_model/`: IOMMU功能模型实现
- `iommu/iommu_perf_model/`: 性能统计和分析模块
- `iommu/cache_src/`: 缓存子系统（DC/PC/PT/Walker/MSIPT五类缓存）
- `rp/`: RISC-V处理器模拟测试
- `pcienoc/`: PCIe NoC模拟
- `slink/`: SLINK路径模拟
- `ddr/`: DDR内存模型

### 4. 构建脚本体系
- **`build_cpp.sh`**: 直接编译cache_src模块并链接，用于增量开发
- **`compile_and_test.sh`**: 编译指定文件并运行测试，快速验证修改
- **`build_wsl.sh`**: WSL跨平台构建脚本，在WSL本地文件系统编译后拷回Windows
- **`link_and_run.sh`**: 链接并运行仿真程序
- **`test_*.sh`**: 各种测试场景的自动化脚本

### 5. 测试与验证
- **单元测试**: `make test_dedup_unit` 编译并运行PT Cache去重+预取单元测试
- **集成测试**: `make test_dedup_integration` 运行完整的集成测试套件
- **场景化测试**: 通过 `make TEST=<scenario>` 选择不同的测试场景
- **分析工具**: `tmp/` 目录下包含大量Python分析脚本用于性能数据分析

### 6. 平台兼容性
- 主要面向Linux环境，通过SystemC库链接
- 提供WSL构建脚本支持Windows用户
- 包含Windows批处理文件 `compile_systemc.bat` 用于Windows环境

### 7. 构建约束与约定
- 所有SystemC头文件必须通过 `-I/usr/include` 包含
- 测试场景名称需遵循命名规范，对应 `rp/test_rp_*.cc` 文件
- 调试宏通过 `DDEBUG_*` 形式控制各模块的调试输出
- 构建产物统一输出到 `build/` 目录，避免污染源码树