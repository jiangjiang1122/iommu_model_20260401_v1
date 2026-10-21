#!/bin/bash
# 16设备×8通道 multidev_mc 读写端口分离(SPLIT=1) 全量重编脚本。
#   make clean -> make TEST=rand4k_msi_mix_multidev_mc MULTIDEV_MC_SPLIT_RW=1
#   回读 -D 宏校验(含 SPLIT/RWQ/SLINK), 备份产物到 /tmp。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1

readback() {  # $1=logfile  然后是需要回读的宏名列表
  local LOG="$1"; shift
  for M in "$@"; do grep -oE "\-D${M}=[0-9]+" "$LOG" | head -1; done
  grep -oE "\-DTEST_CFG_S13_IOVA_512MB" "$LOG" | head -1
}

echo "################ BUILD: multidev_mc N=16 C=8 SPLIT_RW=1 ################"
LOG1=/tmp/build_mdmc_rwsplit_n16c8.log
make clean > "$LOG1" 2>&1
make TEST=rand4k_msi_mix_multidev_mc \
     MULTIDEV_MC_N=16 MULTIDEV_MC_CHAN=8 \
     MULTIDEV_IOVA_MB=512 MULTIDEV_GROUPS=909 MULTIDEV_QUEUE_DEPTH=32 \
     MULTIDEV_RR_START=0 MULTIDEV_D=3 MULTIDEV_PERDEV_WO=1 \
     MULTIDEV_MC_SPLIT_RW=1 MULTIDEV_MC_RWQ_DEPTH=65536 MULTIDEV_MC_SLINK_NS=50 \
     SCENE13_PTW=64 SCENE13_BUFFER=320 SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 \
     DEDUP_ADM=1 DEBUG=1 -j6 >> "$LOG1" 2>&1
echo "MAKE_EXIT=$?  error_count=$(grep -c 'error:' "$LOG1")"
echo "--- compile errors (if any) ---"
grep -n 'error:' "$LOG1" | head -40
echo "--- macro readback ---"
readback "$LOG1" TEST_CFG_MULTIDEV_MC_SCENE TEST_CFG_MULTI_DEVICE_SCENE TEST_CFG_NUM_DEVICES \
  TEST_CFG_MD_CHAN_PER_DEV TEST_CFG_NUM_PAGES TEST_CFG_MD_QUEUE_DEPTH TEST_CFG_MD_RR_START \
  TEST_CFG_PERDEV_WRITE_ORDER TEST_CFG_PT_DEDUP_PREFETCH_DEPTH TEST_CFG_PT_DEDUP_BUFFER_SIZE \
  TEST_CFG_PTW_MAX_OUTSTANDING_TASKS TEST_CFG_DEDUP_ADMISSION_CTRL \
  TEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING TEST_CFG_IOMMU_READ_MAX_OUTSTANDING TEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING \
  TEST_CFG_SPLIT_RW_PORTS TEST_CFG_MC_RW_QUEUE_DEPTH TEST_CFG_SLINK_NOC_LATENCY_NS
if [ -f iommu_model ]; then
  cp iommu_model /tmp/iommu_model_mdmc_rwsplit_n16c8 && ls -la /tmp/iommu_model_mdmc_rwsplit_n16c8
  echo "BINARY_OK"
else
  echo "BINARY_MISSING"
fi
echo "BUILD_RW_SPLIT_DONE"
