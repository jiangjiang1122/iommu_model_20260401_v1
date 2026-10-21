#!/bin/bash
# 任务2: Steady IOPS / 端口带宽 / DDR分布 / 延时分布 提取
set -u
export LC_ALL=C
f=/tmp/mdmc_rwsplit_n16c8.log
OUT=/tmp/task2_steady.txt
: > "$OUT"

echo "===== [1] Steady IOPS 段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 11 'Steady IOPS:' | head -30 >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] Port Average Bandwidth =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 7 'Port Average Bandwidth' | head -24 >> "$OUT"

echo "" >> "$OUT"
echo "===== [3] Port Bandwidth 稳态/注入期 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 10 '稳态窗口 / 注入期口径' | head -26 >> "$OUT"

echo "" >> "$OUT"
echo "===== [4] Port Task Interval per-port =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 22 'IOMMU Port Task Interval Statistics' | head -30 >> "$OUT"

echo "" >> "$OUT"
echo "===== [5] PTW DDR reads 分布(完整) =====" >> "$OUT"
tail -30000 "$f" | grep -n -B 4 -A 22 'PTW DDR access detail' | head -40 >> "$OUT"

echo "" >> "$OUT"
echo "===== [6] PTW Steady IOPS =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 3 'PTW Steady IOPS' | head -8 >> "$OUT"

echo "" >> "$OUT"
echo "===== [7] Valid Cacheline Occupancy =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 12 'Valid Cacheline Occupancy' | head -16 >> "$OUT"

echo "" >> "$OUT"
echo "===== [8] 4KB Page Concurrency =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 14 '4KB Page Concurrency Statistics' | head -20 >> "$OUT"

echo "" >> "$OUT"
echo "===== [9] PTW Throughput 段 =====" >> "$OUT"
tail -30000 "$f" | grep -n -A 16 'IOMMU / PTW Throughput' | head -20 >> "$OUT"

echo TASK2_STEADY_DONE >> "$OUT"
