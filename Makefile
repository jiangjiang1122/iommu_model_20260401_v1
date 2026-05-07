# Makefile for RISC-V IOMMU SystemC Model

# Compiler settings
CXX = g++
CC = gcc

# SystemC settings - adjust these paths if SystemC is installed elsewhere
# For Linux, using system-installed SystemC
SYSTEMC_PREFIX = /usr
SYSTEMC_INCLUDE = /usr/include
SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu

# Compiler flags
CXXFLAGS = -std=c++17 -w -I$(SYSTEMC_INCLUDE) -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK

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
    iommu/iommu_perf_model/iommu_perf_msipt_cache.cc \
    iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc \
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
    rp/test_rp_thread.cc \
    pcienoc/test_pcienoc.cc \
    ddr/test_ddr.cc 

# CXX_SOURCES = \
#     iommu_top.cc  # Exclude due to missing header files

MAIN_SOURCE = main.cpp

# Object files - place them in the build directory
CXX_OBJECTS = $(patsubst %.cc,build/%.o,$(CXX_SOURCES))
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
	@mkdir -p build build/iommu build/iommu/iommu_fun_model build/iommu/iommu_perf_model build/iommu/cache_src build/iommu/cache_src/common build/iommu/cache_src/cache build/iommu/cache_src/replacement build/iommu/cache_src/subsystem build/rp build/pcienoc build/ddr build/test
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile main source into build directory
build/%.o: %.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@



# Clean build artifacts
clean:
	rm -rf build/*.o $(TARGET) 2>/dev/null || true
	rm -rf build 2>/dev/null || true

# Rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean rebuild

# Dependencies
iommu_command_queue.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh
iommu_translate.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh iommu_req_rsp.hh
iommu_top.o: iommu_top.hh iommu_struct.hh param_trans_def.hh