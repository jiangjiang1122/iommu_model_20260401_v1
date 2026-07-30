# Makefile for RISC-V IOMMU SystemC Model
#
# Test scenario selection:
#   make TEST=rand4k_singlestage      (default) 4KB随机读 + 仅一级地址翻译, 4000 requests, 16MB范围
#   make TEST=seq128k_singlestage     128KB顺序读 + 仅一级地址翻译, 2000 requests, 1MB范围
#   make TEST=sv48_bare               Sv48 + Bare, 1000 requests
#   make TEST=seq128k_twostage        128KB顺序读 + Sv48/Sv48x4两阶段地址翻译, 5000 requests
#   make TEST=rand4k_twostage         4KB随机读 + Sv48/Sv48x4两阶段地址翻译, 4000 requests
#   make test_dedup_unit              PT Cache去重+预取单元测试
#   make test_dedup_integration       PT Cache去重+预取集成测试
#
TEST ?= rand4k_singlestage

# Compiler settings
CXX = g++
CC = gcc

# SystemC settings - adjust these paths if SystemC is installed elsewhere
# For Linux, using system-installed SystemC
SYSTEMC_PREFIX = /usr
SYSTEMC_INCLUDE = /usr/include
SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu

# Compiler flags
CXXFLAGS = -std=c++17 -w -I$(SYSTEMC_INCLUDE) -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -I./slink -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK $(TEST_FLAGS) $(EXTRA_FLAGS)

# Debug/Release build
DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -O0 -DDEBUG -DDEBUG_TRANSLATION -DDEBUG_TWOSTAGE -DDEBUG_MSITRANS -DDEBUG_COMMANDS -DDEBUG_SECONDSTAGE -DDEBUG_ATC -DDEBUG_FAULTS -DDEBUG_INTERRUPT -DDEBUG_HPM -DDEBUG_UTILS
    CFLAGS += -g -O0 -DDEBUG
else
    CXXFLAGS += -O3 -DNDEBUG
    CFLAGS += -O3 -DNDEBUG
endif

# Libraries
LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm

# Test thread file selection and flags based on TEST scenario
# Each scenario has a dedicated source file with clear naming:
#   - test_rp_seq128k_single_stage_thread.cc   : 128KB顺序读 + 单级翻译
#   - test_rp_seq128k_two_stage_thread.cc      : 128KB顺序读 + 两阶段翻译
#   - test_rp_rand4k_single_stage_thread.cc    : 4KB随机读 + 单级翻译
#   - test_rp_sv48_bare_thread.cc              : Sv48+Bare基础测试
ifeq ($(TEST), sv48_bare)
    TEST_THREAD_SRC = rp/test_rp_sv48_bare_thread.cc
    TEST_FLAGS =
else ifeq ($(TEST), seq128k_singlestage)
    TEST_THREAD_SRC = rp/test_rp_seq128k_single_stage_thread.cc
    TEST_FLAGS =
else ifeq ($(TEST), seq128k_twostage)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=0
else ifeq ($(TEST), seq128k_twostage_s2on)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1
else ifeq ($(TEST), seq128k_twostage_s2on_128g)
    # 场景6: 128KB顺序读 + 两阶段 + S2开启 + 128GB/s入口/出口 + 全局并发512 + PTW并发5
    #   基准: 稳态IOPS=250M(100%效率), 纯10000包(跳过Phase1单包)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=5 \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), rand4k_twostage)
    TEST_THREAD_SRC = rp/test_rp_rand4k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1
else ifeq ($(TEST), seq512b_2mb_twostage_s2on)
    # 场景8: 512B步进顺序递增 + VS/G两级均为2MB大页 + 两阶段 + S2开启
    #   复用场景5配置: 64GB/s(512bit)入口/出口 + 全局并发256 + PTW并发4 + D=3预取
    #   大页语义: PT Cache只查不写(命中率0), Walker Cache缓存端到端2MB leaf,
    #   PTW大页不启动预取(仍返回D+1个结果, D个无效), 首包walk最多15次DDR
    SCENE8_PTW ?= 4
    TEST_THREAD_SRC = rp/test_rp_seq512b_2mb_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_512B_2MB -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE8_PTW) \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), seq512b_2mb_twostage_s2on_128g)
    # 场景9: 场景8的高带宽版 - 512B步进顺序递增 + VS/G两级均2MB大页 + 两阶段 + S2开启
    #   128GB/s(1024bit)入口/出口 + 全局并发512 + Buffer512(绑定跟随) + D=3预取
    #   PTW并发可用 make SCENE9_PTW=N 调参(大页任务exec仅~37ns, 默认4理论足够)
    #   目标: 验证大页路径在翻倍带宽下能否翻倍到250M线速
    SCENE9_PTW ?= 4
    TEST_THREAD_SRC = rp/test_rp_seq512b_2mb_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_512B_2MB -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE9_PTW) \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), rand4k_twostage_s2on_128g)
    # 场景7: 4KB随机读(16MB IOVA范围) + 两阶段 + S2开启 + 128GB/s入口/出口
    #   全局并发512 + Buffer512 + PTW并发4(可用 make SCENE7_PTW=N 覆盖调参) + D=3预取(随机IOVA下预取失效) + 10000包(1250页x8)
    SCENE7_PTW ?= 4
    TEST_THREAD_SRC = rp/test_rp_rand4k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE7_PTW) \
                 -DTEST_CFG_NUM_PAGES=1250
else
    # rand4k_singlestage (default)
    TEST_THREAD_SRC = rp/test_rp_rand4k_single_stage_thread.cc
    TEST_FLAGS =
endif

# Source files
CXX_SOURCES = \
    iommu/iommu_fun_model/iommu_utils.cc \
    iommu/iommu_perf_model/iommu_reg.cc \
    iommu/iommu_fun_model/iommu_atc.cc \
    iommu/iommu_fun_model/iommu_ats.cc \
    iommu/iommu_perf_model/iommu_command_queue.cc \
    iommu/iommu_perf_model/iommu_device_context.cc \
    iommu/iommu_fun_model/iommu_faults.cc \
    iommu/iommu_perf_model/iommu_hpm.cc \
    iommu/iommu_fun_model/iommu_interrupt.cc \
    iommu/iommu_fun_model/iommu_msi_trans.cc \
    iommu/iommu_fun_model/iommu_process_context.cc \
    iommu/iommu_perf_model/iommu_ref_api.cc \
    iommu/iommu_fun_model/iommu_second_stage_trans.cc \
    iommu/iommu_fun_model/iommu_two_stage_trans.cc \
    iommu/iommu_fun_model/iommu_translate.cc \
    iommu/iommu_top.cc \
    iommu/iommu_perf_model/iommu_perf_parser.cc \
    iommu/iommu_perf_model/iommu_perf_collector.cc \
    iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc \
    iommu/iommu_perf_model/iommu_perf_xdtw.cc \
    iommu/iommu_perf_model/iommu_perf_ptw.cc \
    iommu/iommu_perf_model/iommu_perf_pt_dedup_flush.cc \
    iommu/iommu_perf_model/iommu_perf_msipt_cache.cc \
    iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc \
    iommu/iommu_perf_model/iommu_perf_reorder.cc \
    iommu/iommu_perf_model/iommu_task_cache_convert.cc \
    iommu/cache_src/common/json_config.cpp \
    iommu/cache_src/common/stats_collector.cpp \
    iommu/cache_src/cache/cache_base.cpp \
    iommu/cache_src/cache/dc_cache.cpp \
    iommu/cache_src/cache/pc_cache.cpp \
    iommu/cache_src/cache/pt_cache.cpp \
    iommu/cache_src/cache/dedup_cache.cpp \
    iommu/cache_src/cache/walker_cache.cpp \
    iommu/cache_src/cache/msipt_cache.cpp \
    iommu/cache_src/replacement/plru_policy.cpp \
    iommu/cache_src/replacement/srrip_policy.cpp \
    iommu/cache_src/subsystem/cache_subsystem.cpp \
    rp/test_rp_func.cc \
    $(TEST_THREAD_SRC) \
    pcienoc/test_pcienoc.cc \
    slink/test_slink.cc \
    ddr/test_ddr.cc 

# CXX_SOURCES = \
#     iommu_top.cc  # Exclude due to missing header files

MAIN_SOURCE = main.cpp

# Object files - place them in the build directory
CXX_OBJECTS = $(patsubst %.cc,build/%.o,$(patsubst %.cpp,build/%.o,$(CXX_SOURCES)))
MAIN_OBJECT = build/$(MAIN_SOURCE:.cpp=.o)
ALL_OBJECTS = $(CXX_OBJECTS) $(MAIN_OBJECT)

# Target executable
TARGET = iommu_model

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(ALL_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -L$(SYSTEMC_LIB) $(LIBS) -Wl,--allow-multiple-definition

# Compile C++ sources into build directory
build/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile C++ sources (.cpp) into build directory
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@



# Clean build artifacts
clean:
	rm -rf build/*.o $(TARGET) 2>/dev/null || true
	rm -rf build 2>/dev/null || true

# Rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean rebuild test_dedup_unit test_dedup_integration

# ===================== Unit Test Target =====================
test_dedup_unit:
	@echo "=========================================="
	@echo "编译 PT Cache去重+预取 单元测试"
	@echo "=========================================="
	g++ -std=c++17 -Wall -Wextra -g \
		-o test_dedup_unit test_dedup_prefetch_unit.cpp
	@echo "✅ 单元测试编译成功"
	@echo ""
	@echo "=========================================="
	@echo "运行单元测试"
	@echo "=========================================="
	./test_dedup_unit

# ===================== Integration Test Target =====================
test_dedup_integration: iommu_model
	@echo "=========================================="
	@echo "运行 PT Cache去重+预取 集成测试"
	@echo "=========================================="
	chmod +x test_integration_dedup_prefetch.sh
	./test_integration_dedup_prefetch.sh

# Dependencies
iommu_command_queue.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh
iommu_translate.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh iommu_req_rsp.hh
iommu_top.o: iommu_top.hh iommu_struct.hh param_trans_def.hh