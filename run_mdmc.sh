#!/bin/bash
# 运行 场景2(单设备8通道, 快) 与 场景1(16设备×8通道, 慢), stdout 落 /tmp(WSL本地盘)。
#   从项目根运行(load_config 相对路径读 JSON)。各自备份 summary CSV, awk 求稳态 IOPS。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1
DONE=/tmp/run_mdmc.DONE
rm -f "$DONE"

# 保留既有 multichan n8 CSV(旧reset前), 防止被本次覆盖
cp -f multichan_n8_512mb_summary.csv multichan_n8_512mb_summary_prevreset.csv 2>/dev/null && \
  echo "preserved prev multichan CSV -> multichan_n8_512mb_summary_prevreset.csv"

run_one() {  # $1=bin $2=log $3=csv $4=csvbackup $5=label
  local BIN="$1" LOG="$2" CSV="$3" BAK="$4" LAB="$5"
  echo "==================== RUN $LAB ===================="
  echo "START $BIN epoch=$(date +%s)"
  timeout 2400 "$BIN" > "$LOG" 2>&1
  echo "RUN_EXIT=$? epoch=$(date +%s)"
  cp -f "$CSV" "$BAK" 2>/dev/null && echo "csv -> $BAK"
  echo "--- ALL steady IOPS (type=4 sum) ---"
  awk -F, '$2==4{s+=$5;n++} END{printf "ALL_steady_MreqS=%.3f (devices=%d)\n",s,n}' "$BAK"
  echo "--- DATA steady IOPS (type=0 sum) ---"
  awk -F, '$2==0{s+=$5;n++} END{printf "DATA_steady_MreqS=%.3f (devices=%d)\n",s,n}' "$BAK"
  echo "--- markers ---"
  grep -m1 -E "MDMC_CONFIG|MC_CONFIG" "$LOG"
  grep -m1 "MDMC_UNIQUE_OK" "$LOG"
  grep -m1 "MD_WINDOW" "$LOG"
  grep -E "MD_FAIRNESS" "$LOG" | head -2
  grep -iE "MULTIDEV_MC_RESULT|MULTICHAN_RESULT" "$LOG" | tail -1
  echo "KB4_ANOMALY count=$(grep -icE 'KB4_ANOMALY' "$LOG")"
}

# 场景2 先跑(快, 验证 multichan reset 改动 + 工具链)
run_one /tmp/iommu_model_mc_n8 /tmp/sim_mc_n8.log \
        multichan_n8_512mb_summary.csv multichan_n8_512mb_summary_reset.csv "场景2 单设备8通道"

# 场景1 (16设备×8通道, 128流, ~17min)
run_one /tmp/iommu_model_mdmc_n16c8 /tmp/sim_mdmc_n16c8.log \
        multidev_mc_n16_c8_512mb_summary.csv multidev_mc_n16_c8_512mb_summary.csv "场景1 16设备x8通道"

touch "$DONE"
echo "RUN_MDMC_DONE"
