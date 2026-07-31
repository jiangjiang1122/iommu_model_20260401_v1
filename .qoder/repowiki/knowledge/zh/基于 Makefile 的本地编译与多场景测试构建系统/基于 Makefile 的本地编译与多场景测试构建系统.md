---
kind: build_system
name: 基于 Makefile 的本地编译与多场景测试构建系统
category: build_system
scope:
    - '**'
source_files:
    - Makefile
    - main.cpp
    - build_cpp.sh
    - compile_and_test.sh
    - link_and_run.sh
---

## 构建系统与工具链

本项目采用 **GNU Make + g++ (C++17)** 作为核心构建系统，依赖 SystemC/TLM-2.0 运行时库，在 Linux 环境下通过 `make` 驱动编译、链接与运行。未发现 Docker、CI/CD、CMake 或跨平台构建配置。

### 1. 构建入口与目标

- **顶层 Makefile**：唯一权威构建脚本，定义所有源文件集合、编译器选项、测试场景与目标产物。
- **默认目标 `all`**：生成可执行文件 `iommu_model`，位于仓库根目录。
- **辅助脚本**：`build_cpp.sh`、`compile_and_test.sh`、`link_and_run.sh` 为历史迭代中遗留的手动构建/调试脚本，功能与 Makefile 重叠，建议统一迁移至 Makefile target。

### 2. 编译器与依赖

| 项目 | 值/路径 |
|---|---|
| C++ 标准 | `-std=c++17` |
| 编译器 | `g++` / `gcc` |
| SystemC 头文件 | `/usr/include` (`SYSTEMC_INCLUDE`) |
| SystemC 库 | `/usr/lib/x86_64-linux-gnu` (`SYSTEMC_LIB`) |
| 链接库 | `-lsystemc -lpthread -lm` |
| 关键宏 | `SC_INCLUDE_DYNAMIC_PROCESSES`, `SC_DISABLE_API_VERSION_CHECK` |
| 调试开关 | `DEBUG=1` 时启用大量 `-DDEBUG_*` 宏（翻译、两阶段、MSI、中断等） |

SystemC 必须已安装到系统路径 `/usr`；若安装在其他位置需修改 `SYSTEMC_PREFIX`、`SYSTEMC_INCLUDE`、`SYSTEMC_LIB` 变量。

### 3. 源码组织与编译规则

- **源文件清单**：`CXX_SOURCES` 显式列出所有 `.cc`/`.cpp` 源文件，覆盖 `iommu/iommu_fun_model`、`iommu/iommu_perf_model`、`iommu/cache_src/*`、`rp/`、`pcienoc/`、`slink/`、`ddr/` 等子模块。
- **对象文件输出**：统一输出到 `build/<对应相对路径>.o`，保持与源码树一致的目录结构，便于定位。
- **编译规则**：`build/%.o: %.cc` 和 `build/%.o: %.cpp` 两条模式规则处理两种扩展名，自动创建目标目录。
- **链接阶段**：使用 `-Wl,--allow-multiple-definition` 允许重复符号（配合 SystemC 动态注册机制），并显式 `-L$(SYSTEMC_LIB)` 指定库路径。

### 4. 测试场景选择机制

通过 `TEST` 变量切换不同测试线程源文件与编译宏：

| TEST 值 | 测试线程源文件 | 关键宏 |
|---|---|---|
| `rand4k_singlestage`（默认） | `rp/test_rp_rand4k_single_stage_thread.cc` | — |
| `seq128k_singlestage` | `rp/test_rp_seq128k_single_stage_thread.cc` | — |
| `sv48_bare` | `rp/test_rp_sv48_bare_thread.cc` | — |
| `seq128k_twostage` | `rp/test_rp_seq128k_two_stage_thread.cc` | `-DTEST_SEQ_128K -DTEST_TWO_STAGE -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=0` |
| `seq128k_twostage_s2on` | 同上 | 额外 `-DTEST_CFG_WALKER_CACHE_S2_ENABLED=1` |
| `rand4k_twostage` | `rp/test_rp_rand4k_two_stage_thread.cc` | `-DTEST_RAND_4K -DTEST_TWO_STAGE -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1` |

用法示例：
```bash
make TEST=rand4k_twostage    # 编译随机4KB两阶段翻译测试
make TEST=seq128k_twostage_s2on  # 顺序128KB + 二级缓存开启
```

### 5. 内置测试 Target

- **`test_dedup_unit`**：独立编译并运行 `test_dedup_prefetch_unit.cpp`，验证 PT Cache 去重+预取逻辑。
- **`test_dedup_integration`**：先确保 `iommu_model` 已构建，再执行 `test_integration_dedup_prefetch.sh` 进行端到端集成测试。

### 6. 清理与重建

- `make clean`：删除 `build/` 目录及 `iommu_model` 可执行文件。
- `make rebuild`：先 clean 再 all。

### 7. 设计决策与约束

- **无自动化 CI/CD**：仓库未包含 GitHub Actions、Jenkinsfile 或 Dockerfile，构建完全依赖开发者本地环境。
- **硬编码路径**：SystemC 路径写死为 `/usr`，移植到其他系统需手动调整。
- **多脚本并存**：除 Makefile 外还存在多个 shell 脚本（`build*.sh`、`compile_and_test.sh`、`link_and_run.sh`），它们复制了相同的编译命令，造成维护冗余。建议逐步废弃这些脚本，将逻辑统一到 Makefile target 中。
- **调试宏泛滥**：`DEBUG=1` 会同时打开十数个 `-DDEBUG_*` 宏，导致二进制体积膨胀且性能下降，仅开发调试时使用。
- **链接器放宽检查**：`--allow-multiple-definition` 掩盖潜在的重复符号问题，应谨慎使用并在最终发布版本中关闭。

### 8. 开发者规范

1. **新增源文件**：必须在 `CXX_SOURCES` 中添加对应条目，否则不会被编译。
2. **新增测试场景**：在 `ifeq ($(TEST), ...)` 分支中添加新场景，遵循现有命名约定（`test_rp_<pattern>_<stage>_thread.cc`）。
3. **避免直接调用 shell 脚本**：优先使用 `make <target>` 方式触发构建，保证一致性。
4. **SystemC 环境准备**：确保 `g++`、`systemc` 头文件和库已安装到 `/usr` 下，或通过修改 Makefile 顶部变量指向自定义安装路径。
5. **调试构建**：使用 `make DEBUG=1` 启用详细日志；发布构建使用 `make DEBUG=0` 以获得优化后的二进制。
