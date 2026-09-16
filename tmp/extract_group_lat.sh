#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 || exit 1
for f in sim_512mb_buf266_ptw27 sim_512mb_buf266_ptw64 sim_scene13_v4_512mb_fix2 sim_512mb_buf512_ptw64; do
  echo "=== LOG: ${f} ==="
  grep -m1 'Buffer Size:' "${f}.log"
  grep -m1 'PTW:                           peak' "${f}.log"
  grep -m1 'PTW total completed tasks:' "${f}.log"
  grep -A4 'PTW Task Group Execution Statistics' "${f}.log" | head -6
done
echo "=== source file mtimes ==="
ls -l --time-style='+%m-%d %H:%M' Makefile rp/test_rp_rand4k_msi_mix_thread.cc iommu/iommu_top.cc iommu/iommu_top.hh iommu/include/iommu_task.hh iommu/cache_src/common/dedup_buffer.h
