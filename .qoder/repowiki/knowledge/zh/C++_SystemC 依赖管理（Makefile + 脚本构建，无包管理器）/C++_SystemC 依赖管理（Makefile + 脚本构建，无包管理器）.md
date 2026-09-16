---
kind: dependency_management
name: C++/SystemC 依赖管理（Makefile + 脚本构建，无包管理器）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - compile_systemc.bat
    - compile_and_test.sh
    - link_and_run.sh
---

本项目未使用任何现代 C++ 包管理器（如 vcpkg、Conan、pkg-config），也未采用 vendoring 或私有仓库策略。第三方依赖完全通过系统安装路径和编译脚本中的硬编码路径引入，属于“手工式”依赖管理。

1. 使用的系统与外部依赖
- SystemC 2.x：作为唯一的第三方库，通过 `-lsystemc` 链接，头文件位于 `/usr/include`，库文件位于 `/usr/lib/x86_64-linux-gnu`（Linux）或 `D:\TOOLS\systemc-2.3.3\install`（Windows，见 compile_systemc.bat）。
- 标准库与平台库：`-lpthread -lm`，均为系统自带。
- 构建工具链：g++ (C++17)、gcc、make、cmake（仅用于编译 SystemC 源码本身）。

2. 关键文件与位置
- Makefile：主构建入口，集中定义 SYSTEMC_PREFIX/SYSTEMC_INCLUDE/SYSTEMC_LIB、CXXFLAGS、LIBS 以及所有源文件列表。
- build_cpp.sh / compile_and_test.sh / link_and_run.sh / test_*.sh：辅助构建脚本，重复声明相同的 -I/-L/-l 参数，与 Makefile 保持同步。
- compile_systemc.bat：在 Windows 上从源码编译并安装 SystemC，设置 SYSTEMC_HOME/SYSTEMC_INCLUDE/SYSTEMC_LIBDIR 环境变量。
- .gitignore：仅忽略 build 目录产物，不跟踪任何依赖清单。

3. 架构与约定
- 依赖声明分散在多个脚本中，没有统一的依赖清单文件；每个脚本都显式写出 `-I/usr/include -L/usr/lib/x86_64-linux-gnu -lsystemc` 等参数。
- SystemC 版本通过宏控制：`-DSC_DISABLE_API_VERSION_CHECK` 关闭 API 版本检查，使模型可兼容不同版本的 SystemC。
- 构建输出统一放在 `build/` 目录，目标可执行文件为 `iommu_model`。
- 测试场景通过 Makefile 的 `TEST=` 变量切换，无需修改依赖配置。

4. 约束与风险
- 强耦合于本地系统路径：若 SystemC 安装路径变化，需同时更新 Makefile 与所有 shell/bat 脚本。
- 无锁版本：不存在 lockfile 或版本锁定机制，不同环境可能链接到不同版本的 SystemC。
- 无依赖校验：构建失败时不会提示缺失依赖，需手动确保系统已安装对应库。
- 多脚本维护成本：新增依赖时需在所有构建脚本中同步添加 `-I/-L/-l` 参数。