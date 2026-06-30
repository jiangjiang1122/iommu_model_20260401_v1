#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

echo "=== DDR arbiter outstanding++ max ==="
grep 'DDR_ARBITER.*outstanding++' $LOG | grep -oP '\-> \K\d+' | sort -n | tail -5

echo ""
echo "=== DDR arbiter outstanding++ distribution ==="
grep 'DDR_ARBITER.*outstanding++' $LOG | grep -oP '\-> \K\d+' | sort -n | uniq -c | sort -rn | head -10

echo ""
echo "=== DDR arbiter total ++/-- ==="
echo "outstanding++: $(grep -c 'DDR_ARBITER.*outstanding++' $LOG)"
echo "outstanding--: $(grep -c 'DDR_ARBITER.*outstanding--' $LOG)"

echo ""
echo "=== PTW_STAT peak (all) ==="
grep 'PTW_STAT' $LOG

echo ""
echo "=== Global outstanding peak progression ==="
grep 'global_outstanding' $LOG | grep -oP 'global_outstanding=\K\d+' | awk '
NR==1{peak=$1; next}
$1>peak{peak=$1; print "New peak: "peak" at line "NR}
'

echo ""
echo "=== Global outstanding histogram (non-256) ==="
grep 'global_outstanding' $LOG | grep -oP 'global_outstanding=\K\d+' | awk '$1<256' | sort -n | uniq -c

echo ""
echo "=== Steady state IOPS 10%-90% ==="
RESP_COUNT=$(grep -c 'send_response_to_initiator' $LOG)
START_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.1}")
END_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.9}")
STEADY_CNT=$((END_CNT - START_CNT))
STEADY_START=$(grep 'send_response_to_initiator' $LOG | sed -n "${START_CNT}p" | grep -oP 'entry=\K[0-9.]+')
STEADY_END=$(grep 'send_response_to_initiator' $LOG | sed -n "${END_CNT}p" | grep -oP 'exit=\K[0-9.]+')
STEADY_DUR=$(awk "BEGIN{printf \"%.1f\", $STEADY_END - $STEADY_START}")
awk -v cnt=$STEADY_CNT -v dur=$STEADY_DUR 'BEGIN{
  iops = cnt / (dur / 1e9)
  printf "Steady(10-90%%): %d resp / %.1f ns = %.4f M IOPS\n", cnt, dur, iops/1e6
}'

echo ""
echo "=== E2E latency distribution ==="
grep 'send_response_to_initiator' $LOG | grep -oP 'e2e=\K[0-9.]+' | awk '
{v=$1; s+=v; c++}
v<=100{b1++}
v>100&&v<=1000{b2++}
v>1000&&v<=2000{b3++}
v>2000&&v<=4000{b4++}
v>4000&&v<=6000{b5++}
v>6000{b6++}
END{
printf "Total: %d, Avg: %.1f ns\n",c,s/c
printf "  <=100ns:   %d (%.1f%%)\n",b1,b1*100.0/c
printf "  100-1k:    %d (%.1f%%)\n",b2,b2*100.0/c
printf "  1k-2k:     %d (%.1f%%)\n",b3,b3*100.0/c
printf "  2k-4k:     %d (%.1f%%)\n",b4,b4*100.0/c
printf "  4k-6k:     %d (%.1f%%)\n",b5,b5*100.0/c
printf "  >6k:       %d (%.1f%%)\n",b6,b6*100.0/c
}'

echo ""
echo "=== Main task only DDR breakdown ==="
echo "--- WC miss (first task) ---"
grep 'task_id=1[^0-9].*GS_EXPLICIT leaf -> DONE' $LOG | grep -oP 'total_reads=\K\d+'
echo "--- WC hit L2 tasks ---"
grep 'GS_EXPLICIT leaf -> DONE.*total_reads=10' $LOG | grep -oP 'task_id=\K\d+'
echo "--- WC hit L3 tasks (5 DDR, main only) ---"
MAIN_5=$(grep 'GS_EXPLICIT leaf -> DONE.*total_reads=5' $LOG | grep -v 'prefetch' | wc -l)
PREFETCH_5=$(grep 'GS_EXPLICIT leaf -> DONE.*total_reads=5' $LOG | grep 'prefetch' | wc -l)
echo "Main tasks with 5 DDR: $MAIN_5"
echo "Prefetch tasks with 5 DDR: $PREFETCH_5"
