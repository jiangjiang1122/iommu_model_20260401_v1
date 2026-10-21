#!/bin/bash
# 任务2详细统计段提取(按绝对行号): CONC_WIN/Buffer/Hold/端口门控/内部并发
set -u
export LC_ALL=C
f=/tmp/mdmc_rwsplit_n16c8.log
OUT=/tmp/task2_detail.txt
: > "$OUT"
TOTAL=$(wc -l < "$f")
echo "TOTAL_LINES=$TOTAL" >> "$OUT"
# tail -30000 行号N -> 绝对行号 = TOTAL-30000+N
B=$((TOTAL-30000))

echo "===== [1] CONC_WIN 注入并发窗口 (tail行23540-23560) =====" >> "$OUT"
sed -n "$((B+23540)),$((B+23560))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] Buffer占用率+Hold分解 (tail行29070-29100) =====" >> "$OUT"
sed -n "$((B+29070)),$((B+29100))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [3] 端口准入 (tail行29450-29470) =====" >> "$OUT"
sed -n "$((B+29450)),$((B+29470))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [4] PTW DDR分布 (tail行29660-29680) =====" >> "$OUT"
sed -n "$((B+29660)),$((B+29680))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [5] PTW延时+间隔 (tail行29820-29858) =====" >> "$OUT"
sed -n "$((B+29820)),$((B+29858))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [6] 端口准入门控统计 (tail行29855-29895) =====" >> "$OUT"
sed -n "$((B+29855)),$((B+29895))p" "$f" >> "$OUT"

echo "" >> "$OUT"
echo "===== [7] Parser快照并发+内部实际并发(时间加权) (tail行29910-29960) =====" >> "$OUT"
sed -n "$((B+29910)),$((B+29960))p" "$f" >> "$OUT"

echo TASK2_DETAIL_DONE >> "$OUT"
