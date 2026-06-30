#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=== 1. DDR DISPATCH 时序 (前60条) ==="
grep 'DDR_DISPATCH' $LOG | head -60 | cat -n

echo ""
echo "=== 2. DDR_ARB 路由时序 (前80条, 含task_id) ==="
grep 'DDR_ARBITER.*Route' $LOG | head -80 | cat -n

echo ""
echo "=== 3. 验证组内并发: Group 1 prefetch DDR dispatch间隔 ==="
echo "Group 1 prefetch tasks (2,3,4) first DDR reads:"
grep 'DDR_DISPATCH.*0xfa80' $LOG | head -3

echo ""
echo "Group 1 prefetch tasks (2,3,4) second DDR reads:"
grep 'DDR_DISPATCH.*addr=0xf0000' $LOG | sed -n '1,3p'

echo ""
echo "=== 4. 验证组间并发: Groups 2-5 main tasks ==="
echo "Tasks 33, 65, 97, 129 DDR reads:"
for tid in 33 65 97 129; do
    echo "--- task_id=$tid ---"
    grep "DDR_ARBITER.*task_id=$tid" $LOG | head -6
done

echo ""
echo "=== 5. 验证prefetch组间并发: Tasks 172,173,174 (Group 2 prefetch) ==="
for tid in 172 173 174; do
    echo "--- task_id=$tid ---"
    grep "DDR_ARBITER.*task_id=$tid" $LOG | head -6
done

echo ""
echo "=== 6. DDR_DISPATCH round间隔统计 ==="
echo "连续DDR_DISPATCH的时间差(ns):"
grep 'DDR_DISPATCH' $LOG | grep -oP 'arrive=\K[0-9]+' | head -60 | awk 'NR>1{print NR-1": "($1-prev)" ns"} {prev=$1}'

echo ""
echo "=== 7. PTW outstanding变化 ==="
grep 'PTW_STAT.*peak\|PTW_STAT.*New peak' $LOG | head -10

echo ""
echo "=== 8. DDR arbiter outstanding峰值 ==="
grep 'DDR_ARBITER.*outstanding++' $LOG | grep -oP '-> \K\d+' | sort -n | uniq -c | sort -rn | head -10
