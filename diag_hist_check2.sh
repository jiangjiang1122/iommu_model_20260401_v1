#!/bin/bash
# 历史场景运行结果 + 竞态路径活跃度核查
set -u
export LC_ALL=C
OUT=/tmp/hist_check2.txt
: > "$OUT"

echo "===== [1] 10/13 mdmc 首测 sim_mdmc_n16c8.log =====" >> "$OUT"
f=/tmp/sim_mdmc_n16c8.log
if [ -f "$f" ]; then
  grep -m 2 'MDMC_CONFIG' "$f" >> "$OUT"
  grep -m 2 -E 'MULTIDEV_MC_RESULT|MULTIDEV_RESULT' "$f" >> "$OUT"
  echo "Fatal_count=$(grep -cE 'Fatal|SC_REPORT_FATAL' "$f")" >> "$OUT"
  grep -m 2 'MD_FAIRNESS' "$f" >> "$OUT"
  echo "DEDUP_UPDATE_main_flush=$(grep -c 'main placeholder flushed' "$f")" >> "$OUT"
  echo "DEDUP_UPDATE_pf_clear=$(grep -c 'prefetch placeholder cleared' "$f")" >> "$OUT"
  echo "lines=$(wc -l < "$f")" >> "$OUT"
else
  echo NOT_FOUND >> "$OUT"
fi

echo "" >> "$OUT"
echo "===== [2] 9月 旧框架 N=8 sim_multidev_n8_512mb_ptw64_buf320_dcfix.log =====" >> "$OUT"
f=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim_multidev_n8_512mb_ptw64_buf320_dcfix.log
if [ -f "$f" ]; then
  grep -m 2 'MD_CONFIG' "$f" >> "$OUT"
  grep -m 2 -E 'MULTIDEV_RESULT' "$f" >> "$OUT"
  echo "Fatal_count=$(grep -cE 'Fatal|SC_REPORT_FATAL' "$f")" >> "$OUT"
  grep -m 2 'MD_FAIRNESS' "$f" >> "$OUT"
  echo "DEDUP_UPDATE_main_flush=$(grep -c 'main placeholder flushed' "$f")" >> "$OUT"
  echo "DEDUP_UPDATE_pf_clear=$(grep -c 'prefetch placeholder cleared' "$f")" >> "$OUT"
  echo "lines=$(wc -l < "$f")" >> "$OUT"
else
  echo NOT_FOUND >> "$OUT"
fi

echo "" >> "$OUT"
echo "===== [3] 10/19 nosplit n16c8 竞态路径活跃度 =====" >> "$OUT"
f=/tmp/mdmc_nosplit_n16c8.log
if [ -f "$f" ]; then
  echo "DEDUP_UPDATE_main_flush=$(grep -c 'main placeholder flushed' "$f")" >> "$OUT"
  echo "DEDUP_UPDATE_pf_clear=$(grep -c 'prefetch placeholder cleared' "$f")" >> "$OUT"
  echo "upgraded_to_main=$(grep -c 'upgraded to main' "$f")" >> "$OUT"
  echo "MISS_inserted=$(grep -c 'inserted main placeholder' "$f")" >> "$OUT"
fi

echo "" >> "$OUT"
echo "===== [4] 10/20 rwsplit(修复后) 同口径计数 =====" >> "$OUT"
f=/tmp/mdmc_rwsplit_n16c8.log
if [ -f "$f" ]; then
  echo "DEDUP_UPDATE_main_flush=$(grep -c 'main placeholder flushed' "$f")" >> "$OUT"
  echo "DEDUP_UPDATE_pf_clear=$(grep -c 'prefetch placeholder cleared' "$f")" >> "$OUT"
  echo "upgraded_to_main=$(grep -c 'upgraded to main' "$f")" >> "$OUT"
  echo "MISS_inserted=$(grep -c 'inserted main placeholder' "$f")" >> "$OUT"
  echo "SKIP_foreign=$(grep -c 'SKIP foreign update' "$f")" >> "$OUT"
fi

echo "" >> "$OUT"
echo "===== [5] 旧崩溃日志(修复前rwsplit)同口径 =====" >> "$OUT"
f=/tmp/mdmc_rerun.log
if [ -f "$f" ]; then
  echo "upgraded_to_main=$(grep -c 'upgraded to main' "$f")" >> "$OUT"
  echo "MISS_inserted=$(grep -c 'inserted main placeholder' "$f")" >> "$OUT"
fi

echo HIST2_DONE >> "$OUT"
