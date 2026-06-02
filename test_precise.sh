#!/bin/bash
# 精确定位临界点 - 基于 DDR=64 的新理论

echo '===== SLINK临界点精确定位测试 ====='
echo '理论临界点: SLINK ≈ 856ns (DDR并发=64)'
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

for ns in 500 800 1000 1200; do
  echo "=== 测试 SLINK=${ns}ns ==="
  
  # 修改参数
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  
  # 强制重新编译
  rm -f build/slink/test_slink.o build/iommu/iommu_perf_model/iommu_perf_ptw.o
  make -j4 > /dev/null 2>&1
  
  # 运行测试
  ./iommu_model > test_precise_${ns}.txt 2>&1
  
  # 提取结果
  steady=$(grep 'Steady IOPS:' test_precise_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' test_precise_${ns}.txt | awk '{print $4}')
  ptw_ddr=$(grep 'PTW  avg DDR reads:' test_precise_${ns}.txt | awk '{print $4}')
  ptw_out=$(grep 'PTW:' test_precise_${ns}.txt | grep 'peak=' | awk '{print $3}')
  ddr_out=$(grep 'DDR (master_1):' test_precise_${ns}.txt | awk '{print $3}')
  
  ddr_total=$((ns * 2 + 100))
  
  echo "  DDR总延时: ${ddr_total}ns"
  echo "  PTW DDR访问: ${ptw_ddr} 次/任务"
  echo "  PTW延时: ${ptw_lat} ns"
  echo "  PTW peak outstanding: ${ptw_out}"
  echo "  DDR peak outstanding: ${ddr_out}"
  echo "  Steady IOPS: ${steady} M/s"
  
  # 判断
  if [ -n "$steady" ]; then
    steady_int=$(echo $steady | awk '{printf "%d", $1}')
    if [ $steady_int -ge 120 ]; then
      echo "  ✅ 性能正常（未达瓶颈）"
    else
      eff=$(echo "$steady 125" | awk '{printf "%.1f", $1/$2*100}')
      echo "  ⚠️  性能下降至 ${eff}%"
    fi
  fi
  echo ''
done

echo '===== 测试完成 ====='
