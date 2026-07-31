---
kind: build_system
name: 基于Makefile的SystemC仿真模型构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
    - build_wsl.sh
    - compile_systemc.bat
    - link_and_run.sh
    - run_wsl.sh
---

该项目使用GNU Make作为核心构建系统，结合多个Shell脚本和批处理文件，支持Linux/WSL/Windows多环境下的SystemC仿真模型编译、测试与运行。

## 构建工具链与依赖
- **编译器**: g++ (C++17标准)，gcc用于C代码
- **仿真框架**: SystemC 2.3.3，通过`-lsystemc`链接，头文件位于`/usr/include`，库文件位于`/usr/lib/x86_64-linux-gnu`
- **线程支持**: `-lpthread`，启用动态进程`-DSC_INCLUDE_DYNAMIC_PROCESSES`
- **调试支持**: 默认DEBUG=1，包含大量DEBUG宏定义用于条件编译

## 构建架构
**Makefile**是主要构建入口，采用模块化设计：
- **目标产物**: `iommu_model`可执行文件，输出到项目根目录
- **对象文件**: 统一放置在`build/`目录下，保持源码目录整洁
- **源文件组织**: 按功能模块划分（`iommu/iommu_fun_model`、`iommu/iommu_perf_model`、`iommu/cache_src`等）

**测试场景系统**通过`TEST`变量控制：
- `rand4k_singlestage`: 4KB随机读+单级地址翻译（默认）
- `seq128k_singlestage`: 128KB顺序读+单级翻译
- `sv48_bare`: Sv48+Bare基础测试
- `seq128k_twostage/rand4k_twostage`: 两阶段地址翻译场景
- 每个场景对应独立的测试线程文件，通过编译时宏定义配置行为

## 多环境支持
**Linux/WSL环境**:
- `build_cpp.sh`: 直接编译cache_src模块并链接
- `compile_and_test.sh`: 增量编译特定文件后链接运行
- `build_wsl.sh`: WSL专用构建脚本，在WSL本地文件系统编译后拷贝回Windows目录，优化跨文件系统性能
- `run_wsl.sh`: WSL环境运行脚本

**Windows环境**:
- `compile_systemc.bat`: 使用CMake编译安装SystemC库，设置环境变量

## 构建流程
1. **编译阶段**: 所有`.cc/.cpp`文件编译为`.o`对象文件，输出到`build/`目录
2. **链接阶段**: 链接SystemC库、pthread、math库，使用`--allow-multiple-definition`允许重复符号
3. **测试阶段**: 内置单元测试(`test_dedup_unit`)和集成测试(`test_dedup_integration`)目标
4. **清理阶段**: `make clean`移除所有构建产物

## 关键约束与约定
- SystemC必须预先安装在`/usr`路径下，或通过环境变量覆盖
- 所有构建脚本硬编码了绝对路径（如`/mnt/d/Qoder_proj/iommu_model_20260401_v1`），限制了移植性
- 构建过程禁用警告(`-w`)，但保留调试信息(`-g`)
- 使用`--no-as-needed`确保未引用库也被链接，避免运行时依赖问题
- 测试场景通过编译时宏定义而非运行时参数配置，便于静态优化

## 扩展机制
- 新增测试场景只需添加对应的线程文件和Makefile中的条件分支
- 新增模块需添加到`CXX_SOURCES`列表
- 支持通过`TEST_FLAGS`变量传递自定义编译选项