#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1

echo "========================================"
echo "  1. REQUEST COUNT"
echo "========================================"
echo "Injected: $(grep -c 'axi_slave_nb_transport_fw.*inbound_fifo' $LOG)"
echo "Completed: $(grep -c 'send_response_to_initiator' $LOG)"
echo "Requested: $(grep 'NUM_REQUESTS\|Sending.*READ' $LOG | head -1)"

echo ""
echo "========================================"
echo "  2. PT Cache Statistics"
echo "========================================"
PT_HIT=$(grep 'PT_LOOKUP response' $LOG | grep -v MSIPT | grep -c 'HIT')
PT_MISS=$(grep 'PT_LOOKUP response' $LOG | grep -v MSIPT | grep -c 'MISS')
PT_TOTAL=$((PT_HIT + PT_MISS))
echo "PT Lookup Total: $PT_TOTAL"
echo "PT HIT: $PT_HIT"
echo "PT MISS: $PT_MISS"
echo "PT Hit Rate: $(awk "BEGIN{printf \"%.2f%%\", $PT_HIT*100.0/$PT_TOTAL}")"
echo "Placeholder HIT (dedup): $(grep -c 'Placeholder HIT' $LOG)"
echo "Dedup Buffer Flush: $(grep -c 'DEDUP_FLUSH_IOVA.*freed' $LOG)"

echo ""
echo "========================================"
echo "  3. PTW Task Count"
echo "========================================"
echo "PTW dispatch (main tasks): $(grep -c 'dispatch to PEQ' $LOG)"
echo "PTW DONE (all tasks): $(grep -c 'leaf -> DONE' $LOG)"
echo "Prefetch tasks spawned: $(grep -c 'Spawned prefetch task' $LOG)"
echo "Prefetch tasks completed: $(grep -c 'Prefetch task.*completed' $LOG)"
echo "Two-stage PF groups: $(grep -c 'Two-stage prefetch, D=' $LOG)"
echo "PF groups completed: $(grep -c 'ALL COMPLETED' $LOG)"

echo ""
echo "========================================"
echo "  5. Walker Cache Statistics"
echo "========================================"
WC_LOOKUP=$(grep -c 'WALKER_LOOKUP request' $LOG)
WC_HIT=$(grep 'WALKER_LOOKUP response' $LOG | grep -c 'HIT')
WC_MISS=$(grep 'WALKER_LOOKUP response' $LOG | grep -c 'MISS')
echo "Walker Lookup Total: $WC_LOOKUP"
echo "Walker HIT: $WC_HIT"
echo "Walker MISS: $WC_MISS"
echo "Walker Hit Rate: $(awk "BEGIN{printf \"%.2f%%\", $WC_HIT*100.0/$WC_LOOKUP}")"
echo ""
echo "Hit Level Distribution:"
grep 'WALKER_LOOKUP response.*HIT' $LOG | grep -oP 'level=\d+' | sort | uniq -c
echo ""
echo "Walker UPDATE count: $(grep -c 'WALKER_UPDATE' $LOG)"
echo ""
echo "DDR reads per PTW task:"
grep 'DONE.*total_reads' $LOG | grep -oP 'total_reads=\d+' | sort | uniq -c | sort -rn
echo ""
echo "First task (WC miss) detail:"
grep 'task_id=1[^0-9].*DONE' $LOG
echo ""
echo "Sample WC hit task detail:"
grep 'task_id=33[^0-9].*DONE' $LOG
grep 'task_id=65[^0-9].*DONE' $LOG

echo ""
echo "========================================"
echo "  6. IOPS & Timing"
echo "========================================"
grep "send_response_to_initiator" $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '
BEGIN{s=0;c=0;mi=999999;ma=0}
{s+=$1;c++;if($1<mi)mi=$1;if($1>ma)ma=$1}
END{
  printf "Total responses: %d\n",c;
  printf "Avg E2E latency: %.1f ns\n",s/c;
  printf "Min E2E latency: %.1f ns\n",mi;
  printf "Max E2E latency: %.1f ns\n",ma;
}'
echo ""
echo "First entry time:"
grep 'send_response_to_initiator' $LOG | head -1 | grep -oP 'entry=\K[0-9.]+' 
echo "Last exit time:"
grep 'send_response_to_initiator' $LOG | tail -1 | grep -oP 'exit=\K[0-9.]+'
echo ""
echo "First injection time:"
grep '\[t=.*inbound_fifo' $LOG | head -1 | grep -oP '\[t=\K[0-9]+'
echo "Last injection time:"
grep '\[t=.*inbound_fifo' $LOG | tail -1 | grep -oP '\[t=\K[0-9]+'
echo ""
echo "Last simulation timestamp:"
grep '\[t=' $LOG | tail -1 | grep -oP '\[t=\K[0-9]+'

echo ""
echo "========================================"
echo "  7. Prefetch Effectiveness"
echo "========================================"
echo "Prefetch PT updates sent: $(grep -c 'async PT Cache updates' $LOG)"
TOTAL_PF_UPDATES=$(grep 'async PT Cache updates' $LOG | grep -oP '\d+ async' | awk '{s+=$1}END{print s}')
echo "Total prefetch entries written: $TOTAL_PF_UPDATES"
echo ""
echo "PT HIT breakdown (real data vs placeholder):"
echo "  Real data HIT (from prefetch/PTW update): $(grep 'PT_LOOKUP response.*HIT' $LOG | grep -v MSIPT | wc -l)"
echo "  Placeholder HIT (from dedup): $(grep -c 'Placeholder HIT' $LOG)"

echo ""
echo "========================================"
echo "  8. Two-Stage Prefetch DDR Detail"
echo "========================================"
echo "VS_PTE_READ results (first 10):"
grep 'VS_PTE_READ.*PPN' $LOG | head -10
echo ""
echo "Skip prefetch (invalid PTE): $(grep -c 'skip prefetch' $LOG)"
echo ""
echo "PF group completion sample:"
grep 'ALL COMPLETED' $LOG | head -5
