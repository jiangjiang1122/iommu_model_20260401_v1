#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim6_max1_twostage.log

echo "============================================"
echo "  MAX_OUTSTANDING=1 + 两阶段 详细分析"
echo "============================================"

echo ""
echo "=== 1. 前5组PTW完成统计 ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | head -5

echo ""
echo "=== 2. 第1组(task_id=1, WC miss) DDR读取 ==="
grep 'DDR_ARBITER.*task_id=1\b' $LOG | head -30

echo ""
echo "=== 3. 第1组预取任务spawn ==="
grep 'PTW_2STAGE_PF.*Spawned\|PTW_2STAGE_PF.*task_id=1\b' $LOG | head -10

echo ""
echo "=== 4. 第2组(task_id=33, WC hit) 主任务+预取任务DDR ==="
for tid in 33 172 173 174; do
    echo "--- task_id=$tid ---"
    grep "DDR_ARBITER.*Route.*task_id=$tid\b" $LOG | head -8
done

echo ""
echo "=== 5. DDR_DISPATCH 前80条 (验证并发) ==="
grep 'DDR_DISPATCH' $LOG | head -80

echo ""
echo "=== 6. 所有组的PTW执行延时 ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'entry=\K[0-9.]+|ptw_done=\K[0-9.]+|e2e=\K[0-9.]+' | paste -d',' - - - | head -15

echo ""
echo "=== 7. DDR并发验证: rd_out变化 ==="
grep 'DDR_DISPATCH.*rd_out=[2-9]\|DDR_DISPATCH.*rd_out=[1-9][0-9]' $LOG | head -20
