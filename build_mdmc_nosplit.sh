#!/bin/bash
# SPLIT=0 回退分支一次性构建验证: 确认 multidev_mc 的 #else(单端口直发)分支语法正确、可全量编译。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1
readback() {
  local LOG="$1"; shift
  for M in "$@"; do grep -oE "\-D${M}=[0-9]+" "$LOG" | head -1; done
}
echo "################ BUILD: multidev_mc N=16 C=8 SPLIT_RW=0 (fallback) ################"
LOG0=/tmp/build_mdmc_nosplit_n16c8.log
make clean > "$LOG0" 2>&1
make TEST=rand4k_msi_mix_multidev_mc \
     MULTIDEV_MC_N=16 MULTIDEV_MC_CHAN=8 \
     MULTIDEV_IOVA_MB=512 MULTIDEV_GROUPS=909 MULTIDEV_QUEUE_DEPTH=32 \
     MULTIDEV_RR_START=0 MULTIDEV_D=3 MULTIDEV_PERDEV_WO=1 \
     MULTIDEV_MC_SPLIT_RW=0 MULTIDEV_MC_SLINK_NS=50 \
     SCENE13_PTW=64 SCENE13_BUFFER=320 SCENE13_GLOBAL=640 SCENE13_READ=320 SCENE13_WRITE=320 \
     DEDUP_ADM=1 DEBUG=1 -j6 >> "$LOG0" 2>&1
echo "MAKE_EXIT=$?  error_count=$(grep -c 'error:' "$LOG0")"
echo "--- compile errors (if any) ---"
grep -n 'error:' "$LOG0" | head -40
echo "--- macro readback (expect SPLIT=0) ---"
readback "$LOG0" TEST_CFG_SPLIT_RW_PORTS TEST_CFG_NUM_DEVICES TEST_CFG_MD_CHAN_PER_DEV TEST_CFG_SLINK_NOC_LATENCY_NS
if [ -f iommu_model ]; then
  cp iommu_model /tmp/iommu_model_mdmc_nosplit_n16c8 && echo "BINARY_OK"
else
  echo "BINARY_MISSING"
fi
echo "BUILD_NOSPLIT_DONE"
