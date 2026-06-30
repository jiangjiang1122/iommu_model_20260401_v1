## 1. 构建系统概览
该项目采用 **GNU Make** 作为核心构建工具，配合一系列 **Shell/Bash 脚本** 实现自动化编译、链接、测试执行及结果分析。项目基于 **SystemC/TLM-2.0** 标准库进行硬件行为建模，主要运行在 Linux/WSL (Windows Subsystem for Linux) 环境下。

### 核心特征：
- **单目标构建**：最终生成名为 `iommu_model` 的可执行文件。
- **场景化配置**：通过 `TEST` 变量支持多种仿真场景（如随机访问、顺序访问、两阶段翻译等）的切换。
- **调试友好**：内置详细的 Debug/Release 模式切换，支持细粒度的模块级日志开关。
- **脚本化测试**：提供大量专用脚本用于回归测试、性能统计和故障根因分析。

## 2. 关键构建文件

| 文件/目录 | 作用描述 |
| :--- | :--- |
| `Makefile` | 核心构建脚本，定义编译规则、依赖关系、源文件列表及测试场景宏定义。 |
| `build_cpp.sh` | 手动编译脚本，主要用于快速验证或增量编译缓存子系统代码。 |
| `compile_and_test.sh` | 编译并立即运行特定功能验证的快捷脚本。 |
| `run_wsl.sh` | WSL 环境下的模型启动脚本，负责检查可执行文件存在性。 |
| `compile_systemc.bat` | Windows 批处理脚本，用于从源码编译安装 SystemC 库依赖。 |
| `test_*.sh` | 各类自动化测试脚本（如 `test_1000req.sh`），负责修改参数、运行仿真并解析日志。 |

## 3. 架构与约定

### 3.1 编译器与环境配置
- **编译器**：使用 `g++` (C++) 和 `gcc` (C)。
- **标准**：强制使用 `C++17` 标准 (`-std=c++17`)。
- **SystemC 路径**：默认指向 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu`。若环境不同，需修改 `SYSTEMC_PREFIX`。
- **动态进程支持**：编译时定义 `SC_INCLUDE_DYNAMIC_PROCESSES` 以支持 SystemC 动态线程。

### 3.2 测试场景管理 (Test Scenarios)
`Makefile` 通过 `TEST` 变量控制不同的测试线程文件和预处理器宏：
- `rand4k` (默认): 4KB 随机读，4000 次请求。
- `seq128k`: 128KB 顺序读，2000 次请求。
- `sv48_bare`: Sv48 + Bare 模式，1000 次请求。
- `seq128k_twostage`: 启用两阶段地址翻译及相关缓存去重/预取配置的复杂场景。

### 3.3 调试与日志控制
项目定义了丰富的 Debug 宏，可通过 `DEBUG=1` (默认开启) 激活：
- `DEBUG_TRANSLATION`, `DEBUG_TWOSTAGE`, `DEBUG_MSITRANS`: 地址翻译相关日志。
- `DEBUG_FAULTS`, `DEBUG_INTERRUPT`: 故障与中断日志。
- `DEBUG_HPM`: 性能计数器日志。

### 3.4 单元测试支持
`Makefile` 提供了独立的单元测试目标：
- `make test_dedup_unit`: 编译并运行 PT Cache 去重与预取逻辑的独立单元测试（不依赖 SystemC 仿真环境）。
- `make test_dedup_integration`: 运行集成测试脚本，验证去重逻辑在完整模型中的表现。

## 4. 开发者指南

### 4.1 编译与运行
```bash
# 默认编译 (rand4k 场景, Debug 模式)
make

# 指定场景编译
make TEST=seq128k_twostage

# 清理构建产物
make clean

# 运行模型
./iommu_model
```

### 4.2 新增源文件
若在 `iommu/` 或其子目录下新增 `.cc` 或 `.cpp` 文件，必须将其路径添加到 `Makefile` 的 `CXX_SOURCES` 变量中，否则不会被编译进最终的可执行文件。

### 4.3 跨平台注意事项
- **WSL 用户**：建议使用 `run_wsl.sh` 启动仿真，确保路径映射正确 (`/mnt/d/...`)。
- **Windows 原生**：若需在 Windows 下编译 SystemC 库，可参考 `compile_systemc.bat`，但主模型建议在 WSL 或 Linux 下构建以避免兼容性问题。

### 4.4 性能分析与调试
- 使用 `analyze_*.sh` 和 `analyze_*.py` 脚本对仿真输出的日志进行后处理，提取命中率、延迟分布等关键指标。
- 遇到段错误时，可参考 `GDB_DEBUG_GUIDE.md` 使用 GDB 进行调试。
