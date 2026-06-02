#!/bin/bash
# SLINK延迟临界点测试

echo '===== SLINK延迟临界点测试 ====='
echo '理论临界点: SLINK ≈ 3575ns'
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

for ns in 150 1000 2000 3000 3500 4000; do
  echo "=== 测试 SLINK=${ns}ns ==="
  
  # 修改参数
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  
  # 强制重新编译
  rm -f build/slink/test_slink.o build/iommu/iommu_perf_model/iommu_perf_ptw.o
  make -j4 > /dev/null 2>&1
  
  # 运行测试
  ./iommu_model > test_slink_${ns}.txt 2>&1
  
  # 提取结果
  steady=$(grep 'Steady IOPS:' test_slink_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' test_slink_${ns}.txt | awk '{print $4}')
  ptw_ddr=$(grep 'PTW  avg DDR reads:' test_slink_${ns}.txt | awk '{print $4}')
  
  ddr_total=$((ns * 2 + 100))
  theory_ptw=$(echo "$ptw_ddr $ddr_total" | awk '{printf "%.1f", $1 * $2}')
  
  echo "  DDR总延时: ${ddr_total}ns | PTW实际: ${ptw_lat}ns | PTW理论: ${theory_ptw}ns"
  echo "  Steady IOPS: ${steady} M/s"
  
  # 检测性能下降
  if [ -n "$steady" ]; then
    steady_int=$(echo $steady | awk '{printf "%d", $1}')
    if [ $steady_int -lt 120 ]; then
      echo "  ⚠️  性能下降！临界点在 SLINK=${ns}ns 附近"
      echo ''
      echo '===== 测试完成 ====='
      exit 0
    fi
  fi
  echo ''
done

echo '===== 所有测试点均未触发瓶颈 ====='
