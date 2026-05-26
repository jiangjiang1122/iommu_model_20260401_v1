#!/bin/bash
# Script to compile the IOMMU model in WSL environment

echo "Compiling IOMMU model in WSL environment..."
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
make clean
make all

if [ $? -eq 0 ]; then
    echo "Compilation successful!"
    echo "Binary 'iommu_model' created."
else
    echo "Compilation failed!"
    exit 1
fi