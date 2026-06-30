#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  A. 预取任务 spawn → DONE 延时"
echo "=========================================="

# Extract spawn time and DONE time for each prefetch task
# Spawn: [t=XXXX][PTW_2STAGE_PF] Spawned prefetch task_id=Y
# DONE: [t=XXXX][PTW_RSP] task_id=Y, GS_EXPLICIT leaf -> DONE

# Build spawn time map
grep 'Spawned prefetch task' $LOG | grep -oP '\[t=\K\d+.*task_id=\K\d+' | sed 's/\[PTW_2STAGE_PF\] Spawned prefetch task_id=//' > /tmp/pf_spawn.txt
# Build DONE time map
grep 'GS_EXPLICIT leaf -> DONE' $LOG | grep -oP '\[t=\K\d+.*task_id=\K\d+' | sed 's/\[PTW_RSP\] task_id=//' | sed 's/, GS_EXPLICIT leaf -> DONE.*//' > /tmp/pf_done.txt

echo "Spawn entries: $(wc -l < /tmp/pf_spawn.txt)"
echo "DONE entries: $(wc -l < /tmp/pf_done.txt)"
echo ""
echo "--- Sample spawn/DONE pairs ---"
head -6 /tmp/pf_spawn.txt
echo "..."
head -6 /tmp/pf_done.txt

echo ""
echo "=========================================="
echo "  B. 预取任务延时 (从spawn时间戳计算)"
echo "=========================================="
# Extract [t=spawn_time] and [t=done_time] for each prefetch task
# Spawn format: TIME task_id
# DONE format: TIME task_id
grep 'Spawned prefetch task' $LOG | awk -F'[][]' '{for(i=1;i<=NF;i++) if($i~/t=/) print $i}' | sed 's/t=//' | awk '{print $1}' > /tmp/pf_spawn_t.txt
grep 'Spawned prefetch task' $LOG | grep -oP 'task_id=\K\d+' > /tmp/pf_spawn_id.txt

grep 'GS_EXPLICIT leaf -> DONE' $LOG | awk -F'[][]' '{for(i=1;i<=NF;i++) if($i~/t=/) print $i}' | sed 's/t=//' | awk '{print $1}' > /tmp/pf_done_t.txt
grep 'GS_EXPLICIT leaf -> DONE' $LOG | grep -oP 'task_id=\K\d+' > /tmp/pf_done_id.txt

# Combine and compute latency for prefetch tasks
paste /tmp/pf_spawn_id.txt /tmp/pf_spawn_t.txt > /tmp/pf_spawn_map.txt
paste /tmp/pf_done_id.txt /tmp/pf_done_t.txt > /tmp/pf_done_map.txt

# Match by task_id and compute latency
awk 'NR==FNR{spawn[$1]=$2;next} ($1 in spawn){lat=$2-spawn[$1]; if(lat>0) print lat}' /tmp/pf_spawn_map.txt /tmp/pf_done_map.txt | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Prefetch tasks: %d\nAvg latency: %.1f ns\nMin: %.1f ns\nMax: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "=========================================="
echo "  C. 主任务延时 (PTW_STAT)"
echo "=========================================="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if(c==1||$1<mi)mi=$1;if($1>ma)ma=$1}END{printf "Main tasks: %d\nAvg e2e: %.1f ns\nMin: %.1f ns\nMax: %.1f ns\n",c,s/c,mi,ma}'

echo ""
echo "=========================================="
echo "  D. 主任务DDR分布 (PTW_STAT)"
echo "=========================================="
grep 'PTW_STAT.*DDR_reads=' $LOG | grep -oP 'DDR_reads=\K\d+' | sort -n | uniq -c | sort -rn
grep 'PTW_STAT.*DDR_reads=' $LOG | grep -oP 'DDR_reads=\K\d+' | awk '{s+=$1;c++}END{printf "Main tasks: %d, Total DDR: %d, Avg DDR/task: %.2f\n",c,s,s/c}'

echo ""
echo "=========================================="
echo "  E. 全部任务汇总"
echo "=========================================="
echo "--- 全部628个DONE任务 ---"
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{printf "All tasks: %d, Total DDR: %d, Avg DDR/task: %.2f\n",c,s,s/c}'
echo ""
echo "--- 主任务157个 ---"
echo "WC miss (1 task): 24 DDR"
echo "WC hit L2 (4 tasks): 10 DDR each"
echo "WC hit L3 (152 tasks): 5 DDR each"
echo "主任务平均DDR: $(awk 'BEGIN{printf "%.2f",(24+40+760)/157}')"
echo ""
echo "--- 预取471个 ---"
echo "每个预取任务: 5 DDR (1 VS_PTE_READ + 4 GS_EXPLICIT)"
echo "预取总DDR: 471*5=2355"
echo "预取平均DDR: 5.00"
