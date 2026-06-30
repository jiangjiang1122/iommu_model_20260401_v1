## 1. 依赖管理系统
本项目采用传统的 **Makefile** 作为构建和依赖管理的核心工具，未使用现代化的包管理器（如 Conan, vcpkg, CMake FetchContent 等）。所有第三方依赖均通过**系统级安装**的方式引入，并在编译时通过硬编码或环境变量指定的路径进行链接。

### 核心依赖
- **SystemC**: 项目强依赖 Accellera SystemC 库（版本通常为 2.3.x），用于实现事务级建模（TLM）和离散事件仿真。
- **POSIX Threads (pthread)**: 用于支持 SystemC 的动态进程及多线程仿真环境。
- **GCC/G++**: 编译器工具链，要求支持 C++17 标准。

## 2. 关键文件与配置
- **`Makefile`**: 定义了所有的编译规则、包含路径、库路径以及测试场景的选择逻辑。它是依赖声明的唯一集中点。
  - `SYSTEMC_PREFIX`, `SYSTEMC_INCLUDE`, `SYSTEMC_LIB`: 显式指定了 SystemC 库在 Linux 系统中的安装位置（默认为 `/usr` 和 `/usr/lib/x86_64-linux-gnu`）。
  - `LIBS`: 声明了链接时需要的外部库 `-lsystemc -lpthread -lm`。
- **`build_cpp.sh` / `compile_wsl.sh`**: 提供了自动化的编译脚本，内部硬编码了与 `Makefile` 一致的包含路径和库路径，确保在 WSL 环境下的一致性。
- **`README.md`**: 明确了环境依赖要求，指出必须在 WSL (Windows Subsystem for Linux) 环境中运行，并需预先安装 SystemC 库。

## 3. 架构与约定
- **系统级依赖注入**: 项目假设开发者已在操作系统层面完成了 SystemC 的安装。构建系统不负责下载、编译或缓存第三方库的二进制文件。
- **头文件包含约定**: 
  - 使用 `#include <systemc.h>` 或 `#include "systemc.h"` 引入核心仿真库。
  - 使用 `using namespace sc_core;` 简化 SystemC 核心命名空间的访问。
- **构建隔离**: 所有中间对象文件（`.o`）均输出至 `build/` 目录，保持源码树的整洁。依赖关系的追踪主要通过 `Makefile` 中的显式规则（如 `iommu_command_queue.o` 依赖的头文件列表）实现。
- **测试场景依赖**: 通过 `TEST` 变量在编译期注入不同的宏定义（如 `-DTEST_SEQ_128K`），从而在不改变源码的情况下切换不同的测试线程依赖（如 `test_rp_seq128k_single_stage_thread.cc`）。

## 4. 开发者遵循规则
1. **环境预置**: 在编译前，必须确保 WSL 环境中已正确安装 SystemC 库，且库文件位于 `Makefile` 指定的路径下。若安装路径不同，需手动修改 `Makefile` 中的 `SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB` 变量。
2. **统一构建入口**: 推荐使用 `make all` 或提供的 `compile_wsl.sh` 脚本进行构建，避免直接使用 `g++` 命令导致包含路径缺失。
3. **依赖变更管理**: 若引入新的第三方库，需在 `Makefile` 的 `CXXFLAGS` 中添加对应的 `-I` 包含路径，并在 `LIBS` 中添加 `-l` 链接库标识。
4. **清理策略**: 切换测试场景或更新头文件后，建议执行 `make clean` 以清除旧的依赖关系缓存，防止增量编译导致的链接错误。