#!/bin/bash
# 校验修复生效 + 统计关键事件
set -u
export LC_ALL=C
OUT=/tmp/verify_fix.txt
: > "$OUT"
f=/tmp/mdmc_rwsplit_n16c8.log
echo "===== SKIP foreign update 计数(修复守卫触发次数) =====" >> "$OUT"
grep -c "SKIP foreign update" "$f" >> "$OUT" 2>&1
echo "" >> "$OUT"
echo "===== Fatal/require 检查(应为0) =====" >> "$OUT"
grep -cE "Fatal|SC_REPORT_FATAL" "$f" >> "$OUT" 2>&1
echo "" >> "$OUT"
echo "===== 结尾统计段位置 =====" >> "$OUT"
grep -n "MULTIDEV_MC_RESULT\]" "$f" | tail -2 >> "$OUT" 2>&1
echo VERIFY_DONE >> "$OUT"
