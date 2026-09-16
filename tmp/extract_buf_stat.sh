#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 || exit 1
for f in sim_512mb_buf266_ptw27 sim_512mb_buf266_ptw64 sim_512mb_buf512_ptw27 sim_512mb_buf512_ptw64; do
  echo "=== ${f} ==="
  sed -n '/PT Dedup Buffer Statistics/,/^====/p' "${f}.log" | head -16
done
