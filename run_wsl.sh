#!/bin/bash
# Script to run the IOMMU model in WSL environment

echo "Running IOMMU model in WSL environment..."
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

if [ ! -f "./iommu_model" ]; then
    echo "Error: iommu_model executable not found!"
    echo "Please compile the project first using: make all"
    exit 1
fi

echo "Starting IOMMU model..."
./iommu_model "$@"

echo "IOMMU model execution completed."