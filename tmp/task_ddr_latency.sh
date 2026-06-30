#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  1. DDR访问次数 (所有DONE任务)"
echo "=========================================="
echo "--- 全部DONE任务DDR分布 ---"
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | sort -n | uniq -c | sort -rn
echo ""
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{printf "Total DDR: %d, Tasks: %d, Avg: %.2f\n",s,c,s/c}'

echo ""
echo "=========================================="
echo "  2. 主任务 vs 预取任务 DDR区分"
echo "=========================================="

echo "--- 主任务 (非prefetch) ---"
grep 'GS_EXPLICIT leaf -> DONE' $LOG | grep -v 'prefetch' | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{if(c>0)printf "Main tasks: %d, Total DDR: %d, Avg DDR/task: %.2f\n",c,s,s/c; else print "No matches"}'

echo ""
echo "--- 预取任务 ---"
grep 'GS_EXPLICIT leaf -> DONE' $LOG | grep 'prefetch' | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{if(c>0)printf "Prefetch tasks: %d, Total DDR: %d, Avg DDR/task: %.2f\n",c,s,s/c; else print "No matches"}'

echo ""
echo "--- 也通过 XDTW/DCD 区分 ---"
echo "DDT walk tasks (device context):"
grep 'XDTW_RSP.*DONE.*total_ddr_reads' $LOG | grep -oP 'total_ddr_reads=\K\d+' | sort -n | uniq -c

echo ""
echo "=========================================="
echo "  3. 主任务处理延时 (entry → DONE)"
echo "=========================================="
echo "--- 主任务 entry/DONE时间对 ---"
# Extract entry time from dispatch log and DONE time
grep 'dispatch to PEQ' $LOG | grep -oP 'task_id=\K\d+' | head -5
echo "..."
echo ""

echo "--- 从PTW_STAT提取主任务延时 ---"
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Main tasks (from PTW_STAT): %d, Avg e2e: %.1f ns, Min: %.1f ns, Max: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "=========================================="
echo "  4. 预取任务处理延时"
echo "=========================================="
echo "--- 预取任务 spawn → DONE ---"
# Prefetch spawn time
grep 'Spawned prefetch task' $LOG | head -5
echo ""
# Prefetch DONE time
grep 'GS_EXPLICIT leaf -> DONE.*prefetch' $LOG | head -5
echo ""

echo "--- 预取组总延时 ---"
# Each group: first spawn to last DONE
grep 'ALL COMPLETED' $LOG | head -5
echo ""
grep 'ALL COMPLETED' $LOG | grep -oP '\[t=\K\d+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Group completion times: count=%d\n",c}'

echo ""
echo "=========================================="
echo "  5. 每个主任务 (PTW_STAT) 详细延时"
echo "=========================================="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | awk '{
    # Extract entry, ptw_done, e2e
    match($0, /entry=([0-9.]+)/, e);
    match($0, /ptw_done=([0-9.]+)/, p);
    match($0, /e2e=([0-9.]+)/, d);
    match($0, /DDR_reads=([0-9]+)/, r);
    printf "entry=%.0f ns, ptw_done=%.0f ns, e2e=%.0f ns, DDR=%s\n", e[1], p[1], d[1], r[1]
}' | head -20

echo ""
echo "=========================================="
echo "  6. 主任务DDR次数分布 (PTW_STAT)"
echo "=========================================="
grep 'PTW_STAT.*DDR_reads=' $LOG | grep -oP 'DDR_reads=\K\d+' | sort -n | uniq -c | sort -rn
grep 'PTW_STAT.*DDR_reads=' $LOG | grep -oP 'DDR_reads=\K\d+' | awk '{s+=$1;c++}END{printf "PTW_STAT tasks: %d, Total DDR: %d, Avg: %.2f\n",c,s,s/c}'
