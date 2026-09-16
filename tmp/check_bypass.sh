#!/bin/bash
for f in /mnt/d/Qoder_proj/iommu_model_20260401_v1/sim_scene13_v4*.log; do
    echo "=== $f ==="
    grep -i "bypass" "$f"
done
