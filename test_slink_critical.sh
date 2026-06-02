#!/bin/bash
# SLINK延迟阶梯测试 - 精确验证临界点

echo '===== SLINK延迟阶梯测试（精确版）====='
echo '理论临界点预测: SLINK单趟 ≈ 3575 ns'
echo ''

cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

# 测试点：从150ns开始，逐步增加到瓶颈点
for ns in 150 500 1000 1500 2000 2500 3000 3500 4000 5000 6000 8000; do
  echo "=== T: SLINK=${ns}ns (往返=$((ns*2))ns) ==="
  
  # 修改参数
  sed -i "s/SLINK_NOC_LATENCY_NS = .*/SLINK_NOC_LATENCY_NS = ${ns};/" iommu/iommu_perf_model/iommu_perf_params.hh
  
  # 强制重新编译
  rm -f build/iommu/iommu_perf_model/iommu_perf_ptw.o build/iommu/iommu_perf_model/iommu_perf_pt_cache_response.o
  make -j4 > /dev/null 2>&1
  
  # 运行测试
  ./iommu_model > output_slink_${ns}.txt 2>&1
  
  # 提取关键指标
  iommu_iops=$(grep 'IOMMU IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  ptw_iops=$(grep 'PTW  IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  ptw_lat=$(grep 'PTW  avg exec lat:' output_slink_${ns}.txt | awk '{print $4}')
  ptw_ddr=$(grep 'PTW  avg DDR reads:' output_slink_${ns}.txt | awk '{print $4}')
  steady=$(grep 'Steady IOPS:' output_slink_${ns}.txt | awk '{print $3}')
  sim_time=$(grep 'Sim Time:' output_slink_${ns}.txt | awk '{print $3}')
  
  echo "SimTime: ${sim_time} us | IOMMU: ${iommu_iops} | PTW: ${ptw_iops} | PTW_Lat: ${ptw_lat}ns | DDR: ${ptw_ddr} | Steady: ${steady} M/s"
  echo ''
  
  # 检测是否出现性能下降（IOMMU IOPS < 120M）
  if [ "$steady" != "" ]; then
    steady_int=$(echo "$steady" | awk '{printf "%d", $1}')
    if [ "$steady_int" -lt "124" ]; then
      echo "⚠️  性能下降检测点：SLINK=${ns}ns, Steady IOPS=${steady} M/s"
      echo "=== 临界点已找到 ==="
      break
    fi
  fi
done

echo '===== 测试完成 ====='
