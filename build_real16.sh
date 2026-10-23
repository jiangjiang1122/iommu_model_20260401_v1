#!/usr/bin/env bash
# [16盘实测回放] 构建辅助脚本: 独立对象根 build_real16/, 不清理任何既有目录。
# 用法: wsl -e bash /mnt/d/Qoder_proj/iommu_model_20260401_v1/build_real16.sh [fresh|skip]
set -eu -o pipefail
PROJ="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR=/tmp/real16_logs
mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%d_%H%M%S)
FLAGS=()
if [ "${1:-}" = "fresh" ]; then
  echo "[build_real16] --fresh-build: make -B 全量重编(不删除已有目录)"
  FLAGS=(-B)
fi
make -C "$PROJ" ${FLAGS[@]+"${FLAGS[@]}"} -j"$(nproc)" TEST=real16_trace_replay \
  2>&1 | tee "$LOG_DIR/build_real16_$STAMP.log"
echo "[build_real16] DONE -> $PROJ/iommu_model_real16"
