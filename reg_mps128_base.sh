#!/bin/bash
# MPS/MRRS=128 小包回归测试项(任务2基线口径): 单设备 x8通道, 512MB, PTW64/Buf320/R320/W320/G640, ADM1, D3, SLINK50
# 期望稳态 ALL IOPS = 371.64 M/s (前提: 当轮 default_config.json 的 pc_cache = 1ns)
set -e
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
LOG=/tmp/build_reg_mps128_base.log
rm -f "$LOG"

echo "=== [pc_cache readback] 回归前必须回读当轮口径 ==="
grep -A6 '"pc_cache"' iommu/cache_config/default_config.json | grep -E 'arbiter|hash|read_set|compare|_comment'

COMMON="MULTICHAN_N=8 MULTICHAN_IOVA_MB=512 MULTICHAN_GROUPS=909 \
MULTICHAN_QUEUE_DEPTH=32 MULTICHAN_D=3 MULTICHAN_RR_START=0 MULTICHAN_SLINK_NS=50 \
MULTICHAN_LOAD_MODE=fixed_total SCENE13_PTW=64 SCENE13_BUFFER=320 SCENE13_GLOBAL=640 \
SCENE13_READ=320 SCENE13_WRITE=320 DEDUP_ADM=1 DEBUG=1"

echo "=== [1/2] make clean + build MPS128 ==="
make clean >/dev/null 2>&1 || true
make -j6 TEST=rand4k_msi_mix_multichan_mps128 MULTICHAN_MPS=128 $COMMON > "$LOG" 2>&1 \
  || { echo "BUILD FAILED"; tail -40 "$LOG"; exit 1; }

echo "--- macro readback ---"
for m in TEST_CFG_MPS_BYTES TEST_CFG_NUM_CHANNELS TEST_CFG_SPLIT_RW_PORTS \
         TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING TEST_CFG_IOMMU_READ_MAX_OUTSTANDING \
         TEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING TEST_CFG_PTW_MAX_OUTSTANDING_TASKS; do
  grep -o "\-D$m=[0-9]*" "$LOG" | head -1
done
cp -f iommu_model /tmp/iommu_model_reg_mps128_base
ls -l /tmp/iommu_model_reg_mps128_base

echo "=== [2/2] run sim (31816 pkts) ==="
timeout 2400 /tmp/iommu_model_reg_mps128_base > /tmp/sim_reg_mps128_base.log 2>&1 \
  || { echo "RUN FAILED/timeout"; tail -20 /tmp/sim_reg_mps128_base.log; exit 1; }

echo "=== PASS / MD_WINDOW / steady IOPS ==="
grep -aoE "PASS [0-9]+/[0-9]+" /tmp/sim_reg_mps128_base.log | tail -1
grep -m1 "MD_WINDOW" /tmp/sim_reg_mps128_base.log
grep -A 6 "Multichan Overall Steady" /tmp/sim_reg_mps128_base.log | head -10
echo "=== DONE ==="
