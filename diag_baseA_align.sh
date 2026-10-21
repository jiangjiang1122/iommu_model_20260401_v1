#!/bin/bash
# 基线A日志对齐统计段检查: CONC_WIN/Hold分解/内部并发/端口门控
set -u
export LC_ALL=C
f=/tmp/sim_mc_base200ns.log
OUT=/tmp/baseA_align.txt
: > "$OUT"
if [ ! -f "$f" ]; then
  echo "LOG_NOT_FOUND: $f" >> "$OUT"
  echo BASEA_DONE >> "$OUT"
  exit 0
fi
echo "LOG_SIZE=$(stat -c %s "$f") LINES=$(wc -l < "$f")" >> "$OUT"

echo "" >> "$OUT"
echo "===== [A] CONC_WIN / 注入窗口并发段 =====" >> "$OUT"
tail -30000 "$f" | grep -nE 'CONC_WIN|注入窗口内读写并发|时间加权|IOMMU 内部实际并发|Hold Decomposition|持有|端口准入门控|顶满' | head -40 >> "$OUT"

echo "" >> "$OUT"
echo "===== [B] Hold Decomposition 段落 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 8 'Dedup Buffer Hold Decomposition' | head -20 >> "$OUT"

echo "" >> "$OUT"
echo "===== [C] 注入窗口内读写并发顶满程度 全段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 22 '注入窗口内读写并发顶满程度' | head -30 >> "$OUT"

echo "" >> "$OUT"
echo "===== [D] IOMMU 内部实际并发 全段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 14 'IOMMU 内部实际并发' | head -20 >> "$OUT"

echo "" >> "$OUT"
echo "===== [E] 端口准入门控统计 全段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 12 '端口准入门控统计' | head -18 >> "$OUT"

echo "" >> "$OUT"
echo "===== [F] PTW DDR 两阶段分解 =====" >> "$OUT"
tail -30000 "$f" | grep -nE '两阶段分解|late-spawn|预取启动' | head -12 >> "$OUT"

echo BASEA_DONE >> "$OUT"
