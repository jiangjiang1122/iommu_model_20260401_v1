#!/bin/bash
# 基线A: PTW DDR detail / Outstanding Peak / 延时分布 / MD_RP 提取
set -u
export LC_ALL=C
f=/tmp/sim_mc_base200ns.log
OUT=/tmp/baseA_detail.txt
: > "$OUT"

echo "===== [1] PTW DDR access detail =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 10 'PTW DDR access detail' | head -14 >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] PTW DDR reads 分布表 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 14 'DDR_reads    count    percent' | head -18 >> "$OUT"

echo "" >> "$OUT"
echo "===== [3] Outstanding Peak Statistics =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 14 'Outstanding Peak Statistics' | head -18 >> "$OUT"

echo "" >> "$OUT"
echo "===== [4] PTW latency distribution =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 8 'MAIN\]     latency distribution' | head -12 >> "$OUT"

echo "" >> "$OUT"
echo "===== [5] PTW 并发/执行延时全段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 16 'PTW Concurrency & Execution Latency' | head -20 >> "$OUT"

echo "" >> "$OUT"
echo "===== [6] MD_RP / MD_FAIRNESS =====" >> "$OUT"
tail -30000 "$f" | grep -nE 'MD_RP|MD_FAIRNESS|MULTIDEV_RESULT' | head -10 >> "$OUT"

echo "" >> "$OUT"
echo "===== [7] PTW inject/output interval =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 12 'PTW inject interval' | head -16 >> "$OUT"

echo "" >> "$OUT"
echo "===== [8] Buffer 统计段(占用率) =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 14 'PT Dedup Buffer Statistics' | head -18 >> "$OUT"

echo BASEA_DETAIL_DONE >> "$OUT"
