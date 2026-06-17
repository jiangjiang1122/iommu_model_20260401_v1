#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

CXXFLAGS="-std=c++17 -w -I/usr/include -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -I./slink -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK -g -O0 -DDEBUG"

echo "Compiling cache_src .cpp files..."
for f in iommu/cache_src/common/json_config.cpp iommu/cache_src/common/stats_collector.cpp iommu/cache_src/cache/cache_base.cpp iommu/cache_src/cache/dc_cache.cpp iommu/cache_src/cache/pc_cache.cpp iommu/cache_src/cache/pt_cache.cpp iommu/cache_src/cache/walker_cache.cpp iommu/cache_src/cache/msipt_cache.cpp iommu/cache_src/replacement/plru_policy.cpp iommu/cache_src/replacement/srrip_policy.cpp iommu/cache_src/subsystem/cache_subsystem.cpp; do
    outf="build/${f%.cpp}.o"
    mkdir -p "$(dirname "$outf")"
    echo "  Compiling $f -> $outf"
    g++ $CXXFLAGS -c "$f" -o "$outf"
    if [ $? -ne 0 ]; then
        echo "FAILED to compile $f"
        exit 1
    fi
done

echo "Linking..."
OBJS=$(find build -name '*.o' | sort)
g++ $CXXFLAGS -o iommu_model $OBJS -L/usr/lib/x86_64-linux-gnu -lsystemc -Wl,--no-as-needed -lpthread -lm -Wl,--allow-multiple-definition

if [ $? -eq 0 ]; then
    echo "Link successful! iommu_model created."
else
    echo "Link failed!"
    exit 1
fi
