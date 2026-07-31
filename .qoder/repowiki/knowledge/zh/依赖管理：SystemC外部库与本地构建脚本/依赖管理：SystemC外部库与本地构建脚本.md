---
kind: dependency_management
name: 依赖管理：SystemC外部库与本地构建脚本
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - README.md
    - compile_wsl.sh
    - build_cpp.sh
    - link_and_run.sh
---

## 1. 使用的系统与方式
- 本项目为纯 C++/SystemC 仿真模型，**不使用任何包管理器**（无 go.mod、package.json、CMake、vcpkg、Conan 等），所有第三方依赖通过系统级安装 + Makefile 直接链接。
- 唯一的外部依赖是 **SystemC 库**（libsystemc），以静态/动态库形式由系统提供，路径硬编码在 Makefile 中。
- 构建过程完全基于 GNU Make 和 g++/gcc，配合若干 shell/python 辅助脚本完成编译、测试与分析。

## 2. 关键文件与位置
- `Makefile`：集中定义编译器、SystemC 路径、包含目录、源文件列表、目标可执行文件及各类测试目标。
- `README.md`：说明必须在 WSL 环境中运行，并列出 SystemC/GCC/Make 等环境要求。
- `build/`：仅作为对象文件输出目录，为空目录，不存放任何依赖清单或锁文件。
- 根目录下的 `compile_wsl.sh`、`build_cpp.sh`、`link_and_run.sh`、`gdb_debug.sh` 等脚本：封装编译/链接/调试流程，但内部仍调用 g++ 和 make，不涉及依赖解析。
- 各模块头文件通过相对路径 `-I` 参数引入（如 `./iommu/include`、`./iommu/cache_src` 等），属于本地源码依赖，非第三方包。

## 3. 架构与约定
- **无版本锁定机制**：没有 lockfile、vendor 目录或私有仓库配置；SystemC 版本取决于宿主系统 `/usr` 下安装的版本。
- **路径硬编码**：Makefile 中 `SYSTEMC_PREFIX = /usr`、`SYSTEMC_INCLUDE = /usr/include`、`SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu` 固定了 SystemC 的安装位置，跨机器需手动修改。
- **依赖即源码**：所有“依赖”均为本仓库内的 `.cc/.hh` 文件，通过 Makefile 的 `CXX_SOURCES` 变量显式列举参与编译，不存在隐式依赖发现。
- **测试与工具链分离**：单元测试 (`test_dedup_unit`) 和集成测试 (`test_integration_dedup_prefetch.sh`) 通过独立目标/脚本触发，不改变主构建的依赖声明。

## 4. 约定与约束
- **必须使用 WSL/Linux 环境**：README 明确要求所有编译和运行在 WSL2 Ubuntu 中进行，Windows 原生环境不受支持。
- **SystemC 必须预装于 `/usr`**：若安装路径不同，需手动修改 Makefile 中的 `SYSTEMC_INCLUDE` 和 `SYSTEMC_LIB`。
- **C++17 标准**：Makefile 强制 `-std=c++17`，编译器需支持该标准。
- **无第三方包管理**：项目未引入任何包管理器或依赖声明文件，所有外部库均通过系统包管理器（如 apt）安装后由 Makefile 链接。
- **构建产物隔离**：所有中间对象文件输出到 `build/` 目录，保持源码树整洁，便于清理和重复构建。

## 5. 总结
该项目采用最简化的依赖管理模式：依赖即系统库（SystemC）+ 本地源码，通过 Makefile 集中管理编译选项与链接顺序。这种方式的优点是简单透明、无额外工具链开销，缺点是跨环境一致性依赖较弱，需要使用者自行保证 SystemC 版本与路径一致。