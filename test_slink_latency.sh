#!/bin/bash
# SLINK延迟阶梯测试脚本

echo '===== SLINK延迟阶梯测试 ====='
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

for ns in 150 2000 5000 8000 10000 15000 20000; do
  echo "=== T: SLINK=${ns}ns ==="
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  rm -f build/iommu/iommu_perf_model/iommu_perf_ptw.o
  make -j4 > /dev/null 2>&1
  ./iommu_model > output_slink_${ns}.txt 2>&1
  iommu_iops=$(grep 'IOMMU IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  ptw_iops=$(grep 'PTW  IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' output_slink_${ns}.txt | awk '{print $4}')
  steady=$(grep 'Steady IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  echo "IOMMU: ${iommu_iops} M/s | PTW: ${ptw_iops} M/s | PTW Lat: ${ptw_lat} ns | Steady: ${steady} M/s"
  echo ''
done

echo '===== 测试完成 ====='
