#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=========================================="
echo "  PTW Outstanding 统计"
echo "=========================================="
echo "--- PTW peak outstanding ---"
grep 'PTW_STAT.*peak' $LOG
echo ""
echo "--- DDR arbiter (axi_master_1) outstanding ---"
grep 'DDR_ARBITER.*outstanding' $LOG | grep -oP 'outstanding\+\+ -> \K\d+' | sort -n | tail -5
echo "Max DDR arbiter outstanding:"
grep 'DDR_ARBITER.*outstanding++' $LOG | grep -oP '-> \K\d+' | sort -n | tail -1
echo ""
echo "--- Global outstanding distribution ---"
grep 'global_outstanding' $LOG | grep -oP 'global_outstanding=\K\d+' | sort -n | uniq -c | sort -rn | head -10
echo ""
echo "--- Global outstanding peak ---"
grep 'global_outstanding' $LOG | grep -oP 'global_outstanding=\K\d+' | sort -n | tail -1

echo ""
echo "=========================================="
echo "  PTW 任务DDR访问明细"
echo "=========================================="
echo "--- 所有DONE任务DDR分布 ---"
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | sort -n | uniq -c | sort -rn
echo ""
echo "--- 总DDR读次数 ---"
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{printf "Total DDR: %d, tasks: %d, avg: %.2f\n",s,c,s/c}'
echo ""
echo "--- 主任务DDR明细 ---"
echo "WC miss任务 (24次DDR):"
grep 'GS_EXPLICIT leaf -> DONE.*total_reads=24' $LOG | wc -l
echo "WC hit L3主任务 (5次DDR, 包含prefetch group):"
grep 'GS_EXPLICIT leaf -> DONE.*total_reads=5' $LOG | wc -l
echo "WC hit L2主任务 (10次DDR):"
grep 'GS_EXPLICIT leaf -> DONE.*total_reads=10' $LOG | wc -l
echo ""
echo "--- 主任务单独统计 ---"
grep 'GS_EXPLICIT leaf -> DONE' $LOG | grep -v 'prefetch' | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{printf "Main tasks DDR: total=%d, count=%d, avg=%.2f\n",s,c,s/c}'

echo ""
echo "=========================================="
echo "  PT Cache 访问统计"
echo "=========================================="
echo "--- PT_CACHE_EXECUTE 总访问 ---"
TOTAL_EXEC=$(grep -c 'PT_CACHE_EXECUTE' $LOG)
echo "PT_CACHE_EXECUTE total: $TOTAL_EXEC"
echo ""
echo "--- 按类型分布 ---"
echo "  HIT (real data):"
grep 'PT_CACHE_EXECUTE' $LOG | grep -c 'HIT'
echo "  MISS:"
grep 'PT_CACHE_EXECUTE' $LOG | grep -c 'MISS'
echo "  Placeholder HIT:"
grep 'PT_CACHE_EXECUTE' $LOG | grep -c 'Placeholder'
echo ""
echo "--- PT_LOOKUP 总访问 (request侧) ---"
PT_REQ=$(grep 'PT_LOOKUP request' $LOG | grep -v MSIPT | wc -l)
echo "PT_LOOKUP requests: $PT_REQ"
PT_HIT=$(grep 'PT_LOOKUP response' $LOG | grep -v MSIPT | grep -c 'HIT')
PT_MISS=$(grep 'PT_LOOKUP response' $LOG | grep -v MSIPT | grep -c 'MISS')
echo "PT_LOOKUP HIT: $PT_HIT"
echo "PT_LOOKUP MISS: $PT_MISS"
PT_TOTAL=$((PT_HIT + PT_MISS))
echo "PT Cache Hit Rate: $(awk "BEGIN{printf \"%.2f%%\", $PT_HIT*100.0/$PT_TOTAL}")"
echo ""
echo "--- PT Cache 写入 (来自prefetch group + PTW) ---"
echo "Prefetch group updates:"
grep 'PTW_PREFETCH_MONITOR.*sent' $LOG | grep -oP '\d+ async' | awk '{s+=$1}END{printf "Total prefetch PT updates: %d\n",s}'
echo "PTW direct updates (PT_LOOKUP MISS -> fill):"
PT_MISS
echo ""
echo "--- PT Cache 总写入 ---"
grep 'PT_CACHE_FILLREPL\|PT_CACHE_WRITE\|async PT Cache\|fill_repl\|write_cache' $LOG | wc -l

echo ""
echo "=========================================="
echo "  IOMMU Top-level 统计"
echo "=========================================="
echo "--- send_response_to_initiator ---"
grep -c 'send_response_to_initiator' $LOG
echo ""
echo "--- E2E latency stats ---"
grep 'send_response_to_initiator' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++;if($1<mi||c==1)mi=$1;if($1>ma)ma=$1}END{printf "Avg: %.1f ns, Min: %.1f ns, Max: %.1f ns, Count: %d\n",s/c,mi,ma,c}'
