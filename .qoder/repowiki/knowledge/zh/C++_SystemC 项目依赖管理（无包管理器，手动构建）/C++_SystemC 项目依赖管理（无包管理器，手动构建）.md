---
kind: dependency_management
name: C++/SystemC 项目依赖管理（无包管理器，手动构建）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_and_test.sh
---

该仓库是一个基于 SystemC/TLM 的 RISC-V IOMMU 性能模型 C++ 项目，未使用任何现代包管理器或依赖声明文件（如 go.mod、package.json、CMakeLists.txt、vcpkg、Conan、NuGet 等），所有第三方依赖通过系统路径硬编码方式引入。

1. 依赖来源与版本锁定方式
- 唯一外部依赖是 SystemC 库，通过 Makefile 中的 SYSTEMC_PREFIX=/usr、SYSTEMC_INCLUDE=/usr/include、SYSTEMC_LIB=/usr/lib/x86_64-linux-gnu 指向系统安装位置，依赖版本完全取决于宿主系统的 SystemC 安装情况，不存在 lockfile 或版本约束文件。
- 其他链接库 -lpthread -lm 为系统标准库，同样由系统提供。

2. 构建系统与编译配置
- 构建入口为根目录 Makefile，定义 C++17 标准、大量 -I 头文件搜索路径以及调试/发布模式开关（DEBUG=1 时启用大量 DEBUG_* 宏）。
- 辅助脚本 build_cpp.sh、compile_and_test.sh 以手写 g++ 命令形式重复了相同的编译参数，用于快速增量编译和测试。
- 目标可执行文件名为 iommu_model，对象文件统一输出到 build/ 目录。

3. 依赖管理与更新策略
- 没有 vendoring、子模块或私有注册表；SystemC 必须预先安装在 /usr 下，否则构建失败。
- 升级 SystemC 需要手动替换系统安装，并在 Makefile 中确认路径正确，没有任何自动化检测或降级机制。
- 项目中 Python 分析脚本（tmp/*.py、plot_*.py）未声明依赖，默认假设系统已安装所需 Python 环境。

4. 约束与约定
- 所有 .cc/.cpp 源文件通过 CXX_SOURCES 变量显式列举并参与构建，新增源文件需手动添加到该列表。
- 单元测试通过独立的 test_dedup_unit.cpp + g++ 直接编译运行，不集成到主 Makefile 目标。
- 构建过程对多定义符号使用 -Wl,--allow-multiple-definition 放宽链接检查，表明代码中存在跨翻译单元的重复符号定义。