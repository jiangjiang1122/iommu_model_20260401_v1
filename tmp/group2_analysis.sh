#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=== DDR DISPATCH: entries 38-80 (Group 2+ start) ==="
grep 'DDR_DISPATCH' $LOG | sed -n '38,80p' | cat -n

echo ""
echo "=== Group 2 (main=33, prefetch=172,173,174) - ALL DDR arbiter entries ==="
for tid in 33 172 173 174; do
    echo "--- task_id=$tid ---"
    grep "DDR_ARBITER.*Route.*task_id=$tid" $LOG
done

echo ""
echo "=== Group 3 (main=65, prefetch=204,205,206?) - ALL DDR arbiter entries ==="
for tid in 65 204 205 206; do
    echo "--- task_id=$tid ---"
    grep "DDR_ARBITER.*Route.*task_id=$tid" $LOG | head -8
done

echo ""
echo "=== Task spawn log (PTW_2STAGE_PF, PTW_2STAGE_PF_SPAWN) ==="
grep 'PTW_2STAGE_PF\|PTW_WC_HIT\|PTW_VS_LEAF\|PTW_REQ.*task_id=33\|PTW_REQ.*task_id=65\|PTW_SPAWN' $LOG | head -30

echo ""
echo "=== Group completion (monitor) ==="
grep 'PTW_MONITOR.*group.*complete\|PTW_MONITOR.*DONE\|PTW_DONE.*task_id=33\|PTW_DONE.*task_id=65\|PTW_DONE.*task_id=172' $LOG | head -20

echo ""
echo "=== PTW outstanding event timing ==="
grep 'PTW_STAT' $LOG | head -15
