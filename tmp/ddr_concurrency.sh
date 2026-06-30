#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  1. 组级并发分析 (DDR arbiter rd_out)"
echo "=========================================="
echo ""
echo "--- DDR arbiter rd_out分布 ---"
grep 'DDR_DISPATCH.*rd_out=' $LOG | grep -oP 'rd_out=\K\d+' | sort -n | uniq -c | sort -rn | head -15
echo ""
echo "--- DDR arbiter rd_out峰值 ---"
grep 'DDR_DISPATCH.*rd_out=' $LOG | grep -oP 'rd_out=\K\d+' | sort -n | tail -3

echo ""
echo "=========================================="
echo "  2. Group 1 DDR详细时序 (首次WC miss)"
echo "=========================================="
echo "--- 主任务task_id=1 DDR dispatch (前10个) ---"
grep 'DDR_DISPATCH' $LOG | grep -E 'arrive=[0-6][0-9]{3} ns' | head -10

echo ""
echo "--- Group 1 spawn预取后DDR dispatch (5174 ns之后) ---"
grep 'DDR_DISPATCH' $LOG | grep -E 'arrive=[5-7][0-9]{3} ns' | head -20

echo ""
echo "--- Group 1 预取任务DONE时间 ---"
grep 'task_id=[234], GS_EXPLICIT leaf -> DONE' $LOG

echo ""
echo "=========================================="
echo "  3. Group 33 DDR详细时序 (WC hit L3)"
echo "=========================================="
echo "--- task_id=33 entry/DONE ---"
grep 'task_id=33[^0-9].*dispatch to PEQ' $LOG
grep 'task_id=33.*WALK COMPLETE' $LOG
grep 'PTW_STAT.*task_id=33' $LOG

echo ""
echo "--- Group 33 spawn时间 ---"
grep 'Spawned prefetch task_id=17[234]' $LOG

echo ""
echo "--- Group 33 DDR dispatch (7000-10000 ns区间) ---"
grep 'DDR_DISPATCH' $LOG | grep -E 'arrive=[7-9][0-9]{3} ns' | head -25

echo ""
echo "--- Group 33 预取任务DONE ---"
grep 'task_id=17[234], GS_EXPLICIT leaf -> DONE' $LOG

echo ""
echo "=========================================="
echo "  4. Group 65 DDR详细时序 (验证并发)"
echo "=========================================="
echo "--- task_id=65 entry/DONE ---"
grep 'task_id=65[^0-9].*dispatch to PEQ' $LOG
grep 'task_id=65.*WALK COMPLETE' $LOG

echo ""
echo "--- Group 65 spawn ---"
grep 'Spawned prefetch task_id=20[789]' $LOG

echo ""
echo "--- Group 65 预取任务DONE ---"
grep 'task_id=20[789], GS_EXPLICIT leaf -> DONE' $LOG

echo ""
echo "=========================================="
echo "  5. 多组DDR并发交叉验证"
echo "=========================================="
echo "--- DDR arbiter rd_out时间线 (前30个dispatch) ---"
grep 'DDR_DISPATCH.*READ' $LOG | head -30 | awk -F'[][]' '{
    for(i=1;i<=NF;i++) if($i~/t=/) ts=$i;
    match($0,/task_id=([0-9]+)/,tid);
    match($0,/rd_out=([0-9]+)/,rd);
    match($0,/arrive=([0-9.]+)/,arr);
    match($0,/addr=(0x[0-9a-f]+)/,addr);
    printf "%s task=%s arrive=%s addr=%s rd_out=%s\n",ts,tid[1],arr[1],addr[1],rd[1]
}'
