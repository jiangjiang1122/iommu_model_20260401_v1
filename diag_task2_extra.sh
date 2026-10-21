#!/bin/bash
# 任务2补充指标: CONC_WIN时间加权并发 / Hold MAIN·SUSP W1-W3分解 / 端口带宽
set -u
export LC_ALL=C
f=/tmp/mdmc_rwsplit_n16c8.log
OUT=/tmp/task2_extra.txt
: > "$OUT"

echo "===== [A] 尾部统计段标签分布 =====" >> "$OUT"
tail -30000 "$f" | grep -oE '^\[[A-Z_]+\]' | sort | uniq -c | sort -rn | head -40 >> "$OUT"

echo "" >> "$OUT"
echo "===== [B] CONC_WIN 相关 =====" >> "$OUT"
tail -30000 "$f" | grep -nE 'CONC_WIN|时间加权|并发窗口' | head -20 >> "$OUT"

echo "" >> "$OUT"
echo "===== [C] Hold/持有延时 MAIN·SUSP W1-W3 =====" >> "$OUT"
tail -30000 "$f" | grep -nE 'Hold|持有|MAIN|SUSP|W1|W2|W3' | head -60 >> "$OUT"

echo "" >> "$OUT"
echo "===== [D] 端口带宽相关 =====" >> "$OUT"
tail -30000 "$f" | grep -nE '带宽|bandwidth|BW|M req/s|Gbps|端口' | head -40 >> "$OUT"

echo "" >> "$OUT"
echo "===== [E] 内部并发快照 =====" >> "$OUT"
tail -30000 "$f" | grep -nE 'IOMMU 内部实际并发|Parser 视角的快照平均并发|Buffer 实际占用率' | head -20 >> "$OUT"

echo TASK2_EXTRA_DONE >> "$OUT"
