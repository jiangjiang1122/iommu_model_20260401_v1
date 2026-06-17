#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

CXXFLAGS="-std=c++17 -w -I/usr/include -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -I./slink -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK -g -O0 -DDEBUG"

echo "Compiling iommu_top.cc..."
g++ $CXXFLAGS -c iommu/iommu_top.cc -o build/iommu/iommu_top.o
if [ $? -ne 0 ]; then echo "FAILED"; exit 1; fi

echo "Linking..."
OBJS=$(find build -name '*.o' | sort)
g++ $CXXFLAGS -o iommu_model $OBJS -L/usr/lib/x86_64-linux-gnu -lsystemc -Wl,--no-as-needed -lpthread -lm -Wl,--allow-multiple-definition
if [ $? -ne 0 ]; then echo "LINK FAILED"; exit 1; fi

echo "Running..."
./iommu_model 2>&1 | grep -A 15 "RAM Access Path Breakdown"
