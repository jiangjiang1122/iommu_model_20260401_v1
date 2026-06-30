#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  预取任务延时分析 (DDR dispatch时序)"
echo "=========================================="

echo "--- Group 1 (task_id=2,3,4) DDR dispatch时序 ---"
echo "Main task_id=1 DONE at t=5174 ns"
echo ""
echo "task_id=2 DDR dispatches:"
grep 'DDR_DISPATCH.*0xfa808\|DDR_DISPATCH.*0xf0000.*rd_out=1\|DDR_DISPATCH.*0xf5000.*rd_out=1\|DDR_DISPATCH.*0xf6000.*rd_out=1\|DDR_DISPATCH.*0xf7808' $LOG | grep -E 'arrive=5[0-9]{3}' | head -5
echo ""
echo "task_id=2 DONE:"
grep 'task_id=2, GS_EXPLICIT leaf -> DONE' $LOG

echo ""
echo "--- Group 33 (task_id=34,35,36?) 时序 ---"
grep 'Group 33' $LOG | head -3
echo ""
# Find spawn task IDs for group 33
grep 'Spawned prefetch.*task_id=' $LOG | sed -n '2,4p'

echo ""
echo "=========================================="
echo "  预取任务 DDR时序提取"  
echo "=========================================="

echo "--- 按DDR dispatch提取预取任务的DDR延迟 ---"
echo "VS_PTE_READ dispatch → 最后一个GS_EXPLICIT dispatch 间隔"
echo ""

# For each group, extract the first and last DDR dispatch times
echo "--- Group 1 detailed ---"
echo "Spawn time: ~5174 ns"
echo "task_id=2 first DDR: 5224 ns (VS_PTE_READ)"
echo "task_id=2 last DDR:  6032 ns (GS_EXPLICIT L0)"
echo "task_id=2 DDR span:  808 ns"
echo ""
echo "task_id=3 first DDR: 5224.5 ns"
echo "task_id=3 last DDR:  6032.5 ns"
echo "task_id=3 DDR span:  808 ns"
echo ""
echo "task_id=4 first DDR: 5225 ns"
echo "task_id=4 last DDR:  6033 ns"
echo "task_id=4 DDR span:  808 ns"

echo ""
echo "--- 所有预取组的DDR dispatch span ---"
# Extract first and last DDR dispatch for each prefetch group by looking at
# DDR_DISPATCH entries that happen between group spawn and ALL COMPLETED

echo ""
echo "=========================================="
echo "  汇总计算"
echo "=========================================="
echo ""
echo "--- 主任务延时 ---"
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Count: %d, Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "--- 主任务DDR分布 ---"
grep 'PTW_STAT.*DDR_reads=' $LOG | grep -oP 'DDR_reads=\K\d+' | sort -n | uniq -c | sort -rn

echo ""
echo "--- 预取任务DDR (全部5次) ---"
echo "每个预取任务: 5 DDR (固定)"
echo "  DDR #1 (VS_PTE_READ): 到达后~50ns延迟 + 200ns DDR读"
echo "  DDR #2-#5 (GS_EXPLICIT L3→L0): 每个~200ns DDR读"
echo "  3个预取任务交叉执行, 总span ~808-860 ns"
echo ""

echo "--- DDR读延迟模型 ---"
echo "DDR读延迟: ~200 ns/read (从DDR_DISPATCH interval推断)"
echo "任务间交叉: ~0.5-1 ns间隔"
