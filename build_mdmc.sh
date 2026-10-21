#!/bin/bash
# 场景1(多设备×多通道 N16C8) + 场景2(单设备多通道 multichan n8) 编译脚本。
#   从同一份当前源码分别 make clean 全量重编, 回读 -D 宏校验, 备份产物到 /tmp。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1

readback() {  # $1=logfile  然后是需要回读的宏名列表
  local LOG="$1"; shift
  for M in "$@"; do grep -oE "\-D${M}=[0-9]+" "$LOG" | head -1; done
  grep -oE "\-DTEST_CFG_S13_IOVA_512MB" "$LOG" | head -1
}

echo "################ BUILD 场景1: multidev_mc N=16 C=8 ################"
LOG1=/tmp/build_mdmc_n16c8.log
make clean > "$LOG1" 2>&1
make TEST=rand4k_msi_mix_multidev_mc \
     MULTIDEV_MC_N=16 MULTIDEV_MC_CHAN=8 \
     MULTIDEV_IOVA_MB=512 MULTIDEV_GROUPS=909 MULTIDEV_QUEUE_DEPTH=32 \
     MULTIDEV_RR_START=0 MULTIDEV_D=3 MULTIDEV_PERDEV_WO=1 \
     SCENE13_PTW=64 SCENE13_BUFFER=320 SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 \
     DEDUP_ADM=1 DEBUG=1 -j6 >> "$LOG1" 2>&1
echo "MAKE_EXIT_s1=$?  error_count=$(grep -c 'error:' "$LOG1")"
echo "--- macro readback (场景1) ---"
readback "$LOG1" TEST_CFG_MULTIDEV_MC_SCENE TEST_CFG_MULTI_DEVICE_SCENE TEST_CFG_NUM_DEVICES \
  TEST_CFG_MD_CHAN_PER_DEV TEST_CFG_NUM_PAGES TEST_CFG_MD_QUEUE_DEPTH TEST_CFG_MD_RR_START \
  TEST_CFG_PERDEV_WRITE_ORDER TEST_CFG_PT_DEDUP_PREFETCH_DEPTH TEST_CFG_PT_DEDUP_BUFFER_SIZE \
  TEST_CFG_PTW_MAX_OUTSTANDING_TASKS TEST_CFG_DEDUP_ADMISSION_CTRL \
  TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING TEST_CFG_IOMMU_READ_MAX_OUTSTANDING TEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING
cp iommu_model /tmp/iommu_model_mdmc_n16c8 && ls -la /tmp/iommu_model_mdmc_n16c8
echo "BUILD_S1_DONE"

echo "################ BUILD 场景2: multichan n=8 (含reset改动) ################"
LOG2=/tmp/build_mc_n8.log
make clean > "$LOG2" 2>&1
make TEST=rand4k_msi_mix_multichan \
     MULTICHAN_N=8 MULTICHAN_IOVA_MB=512 MULTICHAN_GROUPS=909 MULTICHAN_QUEUE_DEPTH=32 \
     MULTICHAN_D=3 MULTICHAN_RR_START=0 MULTICHAN_SLINK_NS=50 MULTICHAN_LOAD_MODE=fixed_total \
     SCENE13_PTW=64 SCENE13_BUFFER=320 SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 \
     DEDUP_ADM=1 DEBUG=1 -j6 >> "$LOG2" 2>&1
echo "MAKE_EXIT_s2=$?  error_count=$(grep -c 'error:' "$LOG2")"
echo "--- macro readback (场景2) ---"
readback "$LOG2" TEST_CFG_MULTI_CHANNEL_SCENE TEST_CFG_MULTI_DEVICE_SCENE TEST_CFG_NUM_CHANNELS \
  TEST_CFG_NUM_DEVICES TEST_CFG_NUM_PAGES TEST_CFG_MC_LOAD_MODE TEST_CFG_MC_QUEUE_DEPTH \
  TEST_CFG_PT_DEDUP_PREFETCH_DEPTH TEST_CFG_PT_DEDUP_BUFFER_SIZE TEST_CFG_PTW_MAX_OUTSTANDING_TASKS \
  TEST_CFG_DEDUP_ADMISSION_CTRL TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING \
  TEST_CFG_IOMMU_READ_MAX_OUTSTANDING TEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING TEST_CFG_SLINK_NOC_LATENCY_NS
cp iommu_model /tmp/iommu_model_mc_n8 && ls -la /tmp/iommu_model_mc_n8
echo "BUILD_S2_DONE"
echo "ALL_BUILDS_DONE"
