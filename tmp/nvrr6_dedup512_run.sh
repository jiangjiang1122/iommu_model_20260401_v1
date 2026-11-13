#!/usr/bin/env bash
# [去重Cache 256->512 A/B 20261112] 复用 256 基线的同一批二进制(sha 已逐位校验),
#   唯一变量 = 运行目录 JSON 的 dedup_cache.num_sets 64->128 (cacheline 256->512)。
#   跳过 build: 几何是运行时 JSON 读取, 非编译期宏。
set -uo pipefail
export NVRR6_TS=dedup512_20261112
export NVRR6_NAMESUF=_pf1024
export NVRR6_DEDUP_SETS=128
export NVRR6_S8_TASKS=128          # 与基线 s8 对齐: 128任务/设备 = 20480包
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 || exit 1

B0=/tmp/nvrr6_$NVRR6_TS
mkdir -p "$B0"
# 二进制与基线逐位相同, 直接带入基线 sha 记录供归档对账
cp /tmp/nvrr6_ptfifo1024_20261109/binary_sha256.txt "$B0/" 2>/dev/null || true

for st in run gates report archive; do
  echo "################ STAGE=$st $(date '+%F %T')"
  bash tmp/nvrr6_matrix_run_all.sh "$st" || { echo "!! STAGE $st FAILED"; exit 1; }
done
echo "################ ALL DONE $(date '+%F %T')"
