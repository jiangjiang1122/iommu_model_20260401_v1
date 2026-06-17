#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

echo "Linking..."
OBJS=$(find build -name '*.o' | sort)
g++ -std=c++17 -w -I/usr/include -g -O0 -DDEBUG -o iommu_model $OBJS -L/usr/lib/x86_64-linux-gnu -lsystemc -Wl,--no-as-needed -lpthread -lm -Wl,--allow-multiple-definition

if [ $? -eq 0 ]; then
    echo "Link successful!"
    echo ""
    echo "Running IOMMU model..."
    ./iommu_model 2>&1 | tail -80
else
    echo "Link failed!"
    exit 1
fi
