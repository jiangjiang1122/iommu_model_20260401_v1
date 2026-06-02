#!/bin/bash
# SLINK延迟阶梯测试 - 验证PTW延时和IOPS

echo '===== SLINK延迟阶梯测试 ====='
echo '理论临界点: SLINK ≈ 3575ns (DDR总延时 ≈ 7250ns)'
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

# 测试点
for ns in 150 2000 3000 4000 5000 6000 8000 10000; do
  ddr_total=$((100 + ns * 2))
  echo "=== T: SLINK=${ns}ns, DDR总=${ddr_total}ns ==="
  
  # 同步修改两个文件
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  sed -i "s/SLINK_NOC_LATENCY_NS      = .*/SLINK_NOC_LATENCY_NS      = ${ns};/" ddr/test_ddr.hh
  
  # 强制重新编译
  rm -f build/ddr/test_ddr.o build/iommu/iommu_perf_model/iommu_perf_ptw.o
  make -j4 > /dev/null 2>&1
  
  # 运行测试
  ./iommu_model > output_verify_${ns}.txt 2>&1
  
  # 提取指标
  steady=$(grep 'Steady IOPS:' output_verify_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' output_verify_${ns}.txt | awk '{print $4}')
  iommu_iops=$(grep 'IOMMU IOPS:' output_verify_${ns}.txt | awk '{print $3}')
  ptw_iops=$(grep 'PTW  IOPS:' output_verify_${ns}.txt | awk '{print $3}')
  ptw_ddr=$(grep 'PTW  avg DDR reads:' output_verify_${ns}.txt | awk '{print $4}')
  
  echo "  IOMMU: ${iommu_iops} | PTW: ${ptw_iops} | PTW_Lat: ${ptw_lat}ns | DDR: ${ptw_ddr} | Steady: ${steady} M/s"
  
  # 验证 PTW 延时是否符合理论
  if [ "$ptw_lat" != "" ] && [ "$ptw_ddr" != "" ]; then
    theory_lat=$(echo "$ptw_ddr $ddr_total" | awk '{printf "%.1f", $1 * $2}')
    echo "  [验证] 理论PTW延时: ${theory_lat}ns (DDR访问${ptw_ddr}次 × ${ddr_total}ns)"
  fi
  
  # 检测性能下降
  if [ -n "$steady" ]; then
    steady_int=$(echo $steady | awk '{printf "%d", $1}')
    if [ $steady_int -lt 120 ]; then
      echo "  ⚠️  性能下降！SLINK=${ns}ns 成为瓶颈"
      echo "=== 临界点已找到 ==="
      exit 0
    fi
  fi
  echo ''
done

echo '===== 所有测试完成，未触发瓶颈 ====='
