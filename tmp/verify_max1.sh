#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim5_max1_output.log

echo "============================================"
echo "  MAX_OUTSTANDING=1 单组验证分析"
echo "============================================"

echo ""
echo "=== 1. PTW peak outstanding ==="
grep 'PTW_STAT.*peak\|PTW_STAT.*New peak' $LOG

echo ""
echo "=== 2. PTW任务完成统计（two_stage_pf_complete） ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | head -15

echo ""
echo "=== 3. 主任务DDR读取次数分布 ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'DDR_reads=\K\d+' | sort -n | uniq -c

echo ""
echo "=== 4. 主任务E2E延时分布 ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '{sum+=$1; n++; if($1>max)max=$1; if(min==0||$1<min)min=$1} END{printf "Count=%d, Avg=%.1f ns, Min=%.1f ns, Max=%.1f ns\n", n, sum/n, min, max}'

echo ""
echo "=== 5. 预取任务spawn日志（前3组） ==="
grep 'PTW_2STAGE_PF.*Spawned' $LOG | head -12

echo ""
echo "=== 6. DDR_DISPATCH并发验证（rd_out变化） ==="
grep 'DDR_DISPATCH' $LOG | head -50

echo ""
echo "=== 7. DDR_ARB路由（前60条，含task_id） ==="
grep 'DDR_ARBITER.*Route.*PTW' $LOG | head -60

echo ""
echo "=== 8. PTW_STAT entry/ptw_done时间差（=PTW执行延时） ==="
grep 'PTW_STAT.*two_stage_pf_complete' $LOG | grep -oP 'entry=\K[0-9.]+|ptw_done=\K[0-9.]+|e2e=\K[0-9.]+' | paste - - - | awk '{printf "entry=%.0f, ptw_done=%.0f, e2e=%.0f, ptw_exec=%.0f ns\n", $1, $2, $3, $2-$1}' | head -15

echo ""
echo "=== 9. 全局统计摘要 ==="
grep 'PTW.*IOPS\|PTW.*avg\|PTW.*Max\|PTW.*Min\|IOMMU.*IOPS\|Steady\|avg e2e' $LOG
