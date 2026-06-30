#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

echo "=== Main Task DDR Read Distribution (from PTW_STAT) ==="
grep 'PTW_STAT.*two_stage_pf_complete' sim_5000_twostage_fix.log | grep -oP 'DDR_reads=\d+' | sed 's/DDR_reads=//g' | sort | uniq -c

echo ""
echo "=== Prefetch Task Count ==="
grep 'Spawned prefetch task_id' sim_5000_twostage_fix.log | wc -l

echo ""
echo "=== Prefetch Tasks with Valid PTE (completed full walk) ==="
grep 'PTW_2STAGE_PF.*VS_PTE_READ.*V=1' sim_5000_twostage_fix.log | wc -l

echo ""
echo "=== Prefetch Tasks with Invalid PTE (early exit) ==="
grep 'VS PTE invalid, skip prefetch' sim_5000_twostage_fix.log | wc -l

echo ""
echo "=== Total PTW DDR reads ==="
grep 'PTW total DDR reads' sim_5000_twostage_fix.log

echo ""
echo "=== Calculation ==="
MAIN_TASKS=583
PREFETCH_TASKS=1500
TOTAL_TASKS=$((MAIN_TASKS + PREFETCH_TASKS))
MAIN_DDR=3036
# Prefetch tasks with valid PTE do 5 reads, invalid PTE do 1 read
VALID_PF=$(grep 'PTW_2STAGE_PF.*VS_PTE_READ.*V=1' sim_5000_twostage_fix.log | wc -l)
INVALID_PF=$(grep 'VS PTE invalid, skip prefetch' sim_5000_twostage_fix.log | wc -l)
PREFETCH_DDR=$((VALID_PF * 5 + INVALID_PF * 1))
TOTAL_DDR=$((MAIN_DDR + PREFETCH_DDR))
AVG_DDR=$(echo "scale=2; $TOTAL_DDR / $TOTAL_TASKS" | bc)

echo "Main tasks: $MAIN_TASKS, DDR reads: $MAIN_DDR"
echo "Prefetch tasks: $PREFETCH_TASKS (valid=$VALID_PF, invalid=$INVALID_PF)"
echo "Prefetch DDR reads: $VALID_PF * 5 + $INVALID_PF * 1 = $PREFETCH_DDR"
echo "Total tasks: $TOTAL_TASKS"
echo "Total DDR reads: $TOTAL_DDR"
echo "Average DDR reads/task (including prefetch): $AVG_DDR"
