#!/bin/bash
# 历史多设备场景运行记录核查: 是否跑过/包数/是否崩溃
set -u
export LC_ALL=C
OUT=/tmp/hist_check.txt
: > "$OUT"

echo "===== /tmp 下 mdmc/multidev 相关日志 =====" >> "$OUT"
ls -la /tmp/*mdmc*.log /tmp/*multidev*.log 2>/dev/null >> "$OUT"

echo "" >> "$OUT"
echo "===== 各日志关键结果行 =====" >> "$OUT"
for f in /tmp/mdmc*.log /tmp/*multidev*.log; do
  [ -f "$f" ] || continue
  echo "--- $f ($(stat -c '%y %s' "$f" | cut -c1-19), $(wc -l < "$f") lines) ---" >> "$OUT"
  grep -m 2 -E 'MDMC_CONFIG|MD_CONFIG|MULTIDEV_MC_RESULT|MULTIDEV_RESULT' "$f" >> "$OUT" 2>&1
  echo "Fatal_count=$(grep -cE 'Fatal|SC_REPORT_FATAL' "$f" 2>/dev/null)" >> "$OUT"
  grep -m 2 -E 'MD_FAIRNESS' "$f" >> "$OUT" 2>&1
done

echo "" >> "$OUT"
echo "===== 项目目录下的旧 mdmc 日志/结果 =====" >> "$OUT"
ls -la /mnt/d/Qoder_proj/iommu_model_20260401_v1/*.log 2>/dev/null | grep -iE 'mdmc|multidev' >> "$OUT"
ls /mnt/d/Qoder_proj/iommu_model_20260401_v1/rwsplit_results/ >> "$OUT"

echo HIST_DONE >> "$OUT"
