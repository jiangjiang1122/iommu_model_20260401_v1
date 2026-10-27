#!/bin/bash
# FIFO深度消融回归: pt_hit/pt_miss_response=32, dedup_request/update=64 (pt_update 保持32)
# 双场景: T1=512B基线(目标286.90) T2=128B小包(目标371.64)
# 每场景 make clean 全量重编, 宏回读, timeout 运行, 稳态IOPS + PASS + FIFO报告提取
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1
COMMON="SCENE13_PTW=64 SCENE13_BUFFER=320 DEDUP_ADM=1 DEBUG=1"

echo "############ BUILD+RUN T1: 单设备8通道 512B 基线 (目标 286.90) ############"
make clean >/tmp/build_t1_fifodep.log 2>&1
make TEST=rand4k_msi_mix_multichan MULTICHAN_N=8 MULTICHAN_IOVA_MB=512 \
  MULTICHAN_GROUPS=909 MULTICHAN_D=3 MULTICHAN_SLINK_NS=50 \
  SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 $COMMON -j6 >>/tmp/build_t1_fifodep.log 2>&1
echo "MAKE_T1_EXIT=$? error_count=$(grep -c 'error:' /tmp/build_t1_fifodep.log)"
[ "$(grep -c 'error:' /tmp/build_t1_fifodep.log)" != "0" ] && { grep -n 'error:' /tmp/build_t1_fifodep.log | head; echo BUILD_T1_FAILED; exit 1; }
cp -f iommu_model /tmp/iommu_model_t1_fifodep
echo "START_T1 epoch=$(date +%s)"
timeout 1500 /tmp/iommu_model_t1_fifodep >/tmp/sim_t1_fifodep.log 2>&1
echo "RUN_T1_EXIT=$? epoch=$(date +%s)"
echo "--- T1 markers ---"
grep -m1 "MC_CONFIG" /tmp/sim_t1_fifodep.log
grep -m1 "MD_WINDOW" /tmp/sim_t1_fifodep.log
grep -aE "MULTICHAN_RESULT\] PASS" /tmp/sim_t1_fifodep.log | tail -1
echo "KB4_ANOMALY_T1=$(grep -ac KB4_ANOMALY /tmp/sim_t1_fifodep.log)"
awk -F, '$2==4{s+=$5} END{printf "T1_ALL_steady_MreqS=%.3f\n",s}' multichan_rwsplit_n8_512mb_summary.csv
echo "--- T1 FIFO report ---"
grep -aA 9 "Entry/Dedup FIFO Utilization" /tmp/sim_t1_fifodep.log

echo
echo "############ BUILD+RUN T2: 单设备8通道 128B 小包 (目标 371.64) ############"
make clean >/tmp/build_t2_fifodep.log 2>&1
make TEST=rand4k_msi_mix_multichan_mps128 MULTICHAN_MPS=128 MULTICHAN_N=8 MULTICHAN_IOVA_MB=512 \
  MULTICHAN_GROUPS=909 MULTICHAN_D=3 MULTICHAN_SLINK_NS=50 \
  SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 $COMMON -j6 >>/tmp/build_t2_fifodep.log 2>&1
echo "MAKE_T2_EXIT=$? error_count=$(grep -c 'error:' /tmp/build_t2_fifodep.log)"
[ "$(grep -c 'error:' /tmp/build_t2_fifodep.log)" != "0" ] && { grep -n 'error:' /tmp/build_t2_fifodep.log | head; echo BUILD_T2_FAILED; exit 1; }
grep -oE "\-DTEST_CFG_MPS_BYTES=[0-9]+" /tmp/build_t2_fifodep.log | head -1
cp -f iommu_model /tmp/iommu_model_t2_fifodep
echo "START_T2 epoch=$(date +%s)"
timeout 1500 /tmp/iommu_model_t2_fifodep >/tmp/sim_t2_fifodep.log 2>&1
echo "RUN_T2_EXIT=$? epoch=$(date +%s)"
echo "--- T2 markers ---"
grep -m1 "MD_WINDOW" /tmp/sim_t2_fifodep.log
grep -aE "MULTICHAN_RESULT\] PASS" /tmp/sim_t2_fifodep.log | tail -1
grep -aE "Steady IOPS|Theory peak|Efficiency" /tmp/sim_t2_fifodep.log | head -3
echo "KB4_ANOMALY_T2=$(grep -ac KB4_ANOMALY /tmp/sim_t2_fifodep.log)"
awk -F, '$2==4{s+=$5} END{printf "T2_ALL_steady_MreqS=%.3f\n",s}' multichan_mps128_rwsplit_n8_512mb_summary.csv
echo "--- T2 FIFO report ---"
grep -aA 9 "Entry/Dedup FIFO Utilization" /tmp/sim_t2_fifodep.log
echo "--- T2 重复走页/PTW ---"
grep -aE "MAIN head \(dedup MISS\)|SUSPENDED \(deduped\)|PTW total completed tasks|Buffer Full Bypass: " /tmp/sim_t2_fifodep.log | head -5
echo "ALL_DONE epoch=$(date +%s)"
