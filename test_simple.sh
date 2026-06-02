#!/bin/bash
# SLINK延迟简化测试 - 5us 和 2.5us

echo '===== SLINK延迟简化测试 ====='
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

for ns in 5000 2500; do
  echo "=== 测试 SLINK=${ns}ns ($(($ns/1000))us) ==="
  
  # 修改参数
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  
  # 强制重新编译
  rm -f build/slink/test_slink.o build/iommu/iommu_perf_model/iommu_perf_ptw.o
  make -j4 > /dev/null 2>&1
  
  # 运行测试
  ./iommu_model > test_simple_${ns}.txt 2>&1
  
  # 提取结果
  steady=$(grep 'Steady IOPS:' test_simple_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' test_simple_${ns}.txt | awk '{print $4}')
  ptw_ddr=$(grep 'PTW  avg DDR reads:' test_simple_${ns}.txt | awk '{print $4}')
  
  ddr_total=$((ns * 2 + 100))
  
  echo "  DDR总延时: ${ddr_total}ns"
  echo "  PTW DDR访问: ${ptw_ddr} 次/任务"
  echo "  PTW实际延时: ${ptw_lat} ns"
  echo "  Steady IOPS: ${steady} M/s"
  
  # 判断是否成为瓶颈
  if [ -n "$steady" ]; then
    steady_int=$(echo $steady | awk '{printf "%d", $1}')
    if [ $steady_int -ge 124 ]; then
      echo "  ✅ 性能正常（无影响）"
    else
      echo "  ⚠️  性能下降（成为瓶颈）"
    fi
  fi
  echo ''
done

echo '===== 测试完成 ====='
