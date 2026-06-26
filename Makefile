# Makefile for RISC-V IOMMU SystemC Model
#
# Test scenario selection:
#   make TEST=rand4k_singlestage      (default) 4KB随机读 + 仅一级地址翻译, 4000 requests, 16MB范围
#   make TEST=seq128k_singlestage     128KB顺序读 + 仅一级地址翻译, 2000 requests, 1MB范围
#   make TEST=sv48_bare               Sv48 + Bare, 1000 requests
#   make TEST=seq128k_twostage        128KB顺序读 + Sv48/Sv48x4两阶段地址翻译, 5000 requests
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
CXXFLAGS = -std=c++17 -w -I$(SYSTEMC_INCLUDE) -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -I./slink -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK $(TEST_FLAGS)

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
ifeq ($(TEST), sv48_bare)
    TEST_THREAD_SRC = rp/test_rp_sv48_bare_thread.cc
    TEST_FLAGS =
else ifeq ($(TEST), seq128k_singlestage)
    TEST_THREAD_SRC = rp/test_rp_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K
else ifeq ($(TEST), seq128k_twostage)
    TEST_THREAD_SRC = rp/test_rp_128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1
else
    # rand4k_singlestage (default)
    TEST_THREAD_SRC = rp/test_rp_thread.cc
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