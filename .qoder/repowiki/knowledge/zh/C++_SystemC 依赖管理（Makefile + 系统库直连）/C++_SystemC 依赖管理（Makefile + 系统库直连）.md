---
kind: dependency_management
name: C++/SystemC 依赖管理（Makefile + 系统库直连）
category: dependency_management
scope:
    - '**'
source_files:
    - Makefile
    - build_cpp.sh
    - README.md
    - iommu/cache_src/common/mini_json.h
---

## 1. 使用的系统与工具
- **构建系统**：GNU Make（根目录 `Makefile`），辅以若干 shell 脚本（`build_cpp.sh`、`compile_and_test.sh`、`link_and_run.sh` 等）作为便捷入口。
- **语言与标准**：C++17，通过 `-std=c++17` 指定。
- **外部依赖**：仅依赖系统级安装的 SystemC/TLM 运行时库（`libsystemc`）以及标准库（pthread、m）。无第三方包管理器（无 go.mod / package.json / vcpkg / conan / CMakeLists.txt / pkg-config 等）。
- **链接选项**：显式使用 `-Wl,--allow-multiple-definition` 和 `-Wl,--no-as-needed` 以兼容 SystemC 动态注册机制。

## 2. 关键文件与位置
- `Makefile`：唯一声明所有源文件、头文件路径、编译/链接选项及目标的可执行文件 `iommu_model` 的清单。
- `build_cpp.sh`：与 Makefile 平行的手工构建脚本，硬编码 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu`，用于快速验证或跨平台调试。
- `README.md`：说明项目依赖 SystemC，并指出 `Makefile` 为构建入口。
- `iommu/cache_src/common/mini_json.h`：自包含 JSON 解析器，注释明确“Self-contained, no external dependencies”，避免引入额外库。
- `Doc/`：存放 IEEE SystemC 1666-2011 与 OSCI TLM 2.0 参考手册 PDF，作为外部规范而非可执行依赖。

## 3. 架构与约定
- **依赖来源**：SystemC/TLM 必须预先安装到系统路径（默认 `/usr/include` + `/usr/lib/x86_64-linux-gnu`），由 `SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量控制；也可通过环境变量覆盖（如 Windows 下 `D:/TOOLS/systemc-2.3.3/install/{include,lib}`）。
- **版本锁定方式**：未使用 lockfile 或包管理器；SystemC 版本由宿主环境决定，构建日志中可见不同机器分别使用了 systemc-2.3.3 与系统包管理器提供的版本。
- **私有仓库/镜像**：未发现任何私有 registry、GOPRIVATE、NPM_REGISTRY 等配置。
- **源码内嵌替代方案**：JSON 解析采用自实现的 `mini_json.h`，避免引入 rapidjson/nlohmann/json 等第三方库，体现“零第三方依赖”的设计倾向。
- **测试场景驱动**：通过 `make TEST=xxx` 选择不同线程源文件与 `TEST_FLAGS` 宏开关，同一份代码在不同编译期配置下生成不同行为，属于“编译期依赖裁剪”而非运行时依赖注入。

## 4. 开发者应遵循的规则
1. **新增外部库需同步修改两处**：在 `Makefile` 的 `CXXFLAGS` 中添加 `-I<path>`，并在 `LIBS` 中添加对应 `-lxxx`，同时更新 `CXX_SOURCES` 列表。
2. **不要引入新的第三方包管理器**：保持当前“系统库 + Makefile 直连”风格，避免引入 vcpkg/conan/CMake 等，除非有强理由。
3. **优先自实现轻量组件**：参考 `mini_json.h` 的做法，对小型工具类尽量内联实现，减少外部依赖面。
4. **跨平台时只改路径变量**：Windows/Linux 差异集中在 `SYSTEMC_PREFIX/INCLUDE/LIB` 与 `CXX`/`CC` 上，不要改动核心编译规则。
5. **谨慎使用 `--allow-multiple-definition`**：该链接选项用于容忍 SystemC 动态注册的重复符号，仅在确认不会引发实际冲突时使用。
6. **单元测试独立于主工程**：`test_dedup_unit` 目标直接调用 g++ 编译单个 `.cpp`，不依赖主工程的对象文件，便于隔离验证。
