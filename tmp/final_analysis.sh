#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  A. 主任务延时详细分布 (PTW_STAT)"
echo "=========================================="
echo ""
echo "--- 按DDR_reads分类的延时 ---"
echo ""
echo "DDR=5 任务:"
grep 'PTW_STAT.*DDR_reads=5' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{if(c>0)printf "  Count: %d, Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo "DDR=10 任务:"
grep 'PTW_STAT.*DDR_reads=10' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{if(c>0)printf "  Count: %d, Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo "DDR=24 任务:"
grep 'PTW_STAT.*DDR_reads=24' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{if(c>0)printf "  Count: %d, Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "--- 全部主任务 ---"
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Count: %d, Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "=========================================="
echo "  B. 预取任务延时估算 (DDR dispatch)"
echo "=========================================="
echo ""
echo "--- DDR_DISPATCH间隔统计 (推断DDR读延迟) ---"
# Extract DDR dispatch times to measure the interval
grep 'DDR_DISPATCH.*READ' $LOG | grep -oP 'dispatch=\K[0-9]+' | head -20 | awk 'NR>1{printf "  Interval: %d ns\n",$1-prev}{prev=$1}'

echo ""
echo "--- 预取组DDR时序分析 ---"
# For each prefetch group, find the first and last DDR dispatch time
# Group 1: spawned at ~5174 ns, tasks 2,3,4
echo "Group 1 (task_id=2,3,4):"
echo "  Spawn: ~5174 ns (main task_id=1 DONE)"
echo "  First DDR dispatch: 5224 ns (task_id=2 VS_PTE_READ)"  
echo "  Last DDR dispatch:  6033 ns (task_id=4 GS_EXPLICIT L0)"
echo "  预取DDR span: 809 ns (15次DDR读交叉执行)"
echo "  单任务DDR延迟: ~1000 ns (5次DDR * 200ns/read)"
echo ""

# Check more groups by looking at DDR dispatch patterns
echo "--- 预取任务DDR dispatch时间戳样本 ---"
echo "Group 33 附近 (spawn at ~9403 ns):"
grep 'DDR_DISPATCH' $LOG | grep -E 'arrive=9[0-9]{3} ns' | head -6
echo ""

echo "--- DDR dispatch 间隔直方图 ---"
grep 'DDR_DISPATCH.*READ' $LOG | grep -oP 'dispatch=\K[0-9]+' | awk '
NR==1{prev=$1; next}
{diff=$1-prev; if(diff>0&&diff<1000) bins[int(diff/50)]++}
prev=$1
END{for(b in bins) printf "  %d-%d ns: %d\n",b*50,(b+1)*50,bins[b]}' | sort -t- -k1 -n | head -10

echo ""
echo "=========================================="
echo "  C. 综合汇总"
echo "=========================================="
echo ""
echo "=== DDR访问次数 ==="
echo "  全部628个任务: avg 5.06 DDR/task"
echo "  157个主任务:   avg 5.25 DDR/task (152×5 + 4×10 + 1×24)"
echo "  471个预取任务: avg 5.00 DDR/task (固定)"
echo ""
echo "=== 处理延时 ==="
echo "  主任务(Walker Cache HIT L3): avg ~2119-4112 ns (5 DDR)"
echo "  主任务(Walker Cache HIT L2): ~DDR=10, 约2×"
echo "  主任务(Walker Cache MISS):   6157 ns (24 DDR)"
echo "  预取任务:                    ~808-1000 ns (5 DDR交叉执行)"
