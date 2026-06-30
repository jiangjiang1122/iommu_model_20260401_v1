#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
LOG=sim4_output.log

echo "=== Steady state config ==="
grep 'steady\|STEADY' rp/test_rp_128k_two_stage_thread.cc

echo ""
echo "=== Timing ==="
FIRST_ENTRY=$(grep 'send_response_to_initiator' $LOG | head -1 | grep -oP 'entry=\K[0-9.]+')
LAST_EXIT=$(grep 'send_response_to_initiator' $LOG | tail -1 | grep -oP 'exit=\K[0-9.]+')
RESP_COUNT=$(grep -c 'send_response_to_initiator' $LOG)
echo "First entry: ${FIRST_ENTRY} ns"
echo "Last exit: ${LAST_EXIT} ns"
echo "Duration: $(awk "BEGIN{printf \"%.1f\", $LAST_EXIT - $FIRST_ENTRY}") ns"
echo "Responses: $RESP_COUNT"

DURATION_NS=$(awk "BEGIN{printf \"%.1f\", $LAST_EXIT - $FIRST_ENTRY}")
echo ""
awk -v cnt=$RESP_COUNT -v dur=$DURATION_NS 'BEGIN{
  iops = cnt / (dur / 1e9)
  printf "IOPS = %d / (%.1f / 1e9) s = %.4f M (%.0f IOPS)\n", cnt, dur, iops/1e6, iops
}'

echo ""
echo "=== PTW Module IOPS ==="
PTW_COUNT=$(grep -c 'dispatch to PEQ' $LOG)
awk -v cnt=$PTW_COUNT -v dur=$DURATION_NS 'BEGIN{
  iops = cnt / (dur / 1e9)
  printf "PTW IOPS = %d main tasks / (%.1f / 1e9) s = %.4f M\n", cnt, dur, iops/1e6
}'

echo ""
echo "=== Steady State IOPS (middle 60%) ==="
# Steady state: 20% to 80% of requests
START_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.2}")
END_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.8}")
echo "Steady state range: response #${START_CNT} to #${END_CNT}"

# Extract entry/exit times for steady state range
STEADY_START=$(grep 'send_response_to_initiator' $LOG | sed -n "${START_CNT}p" | grep -oP 'entry=\K[0-9.]+')
STEADY_END=$(grep 'send_response_to_initiator' $LOG | sed -n "${END_CNT}p" | grep -oP 'exit=\K[0-9.]+')
STEADY_CNT=$((END_CNT - START_CNT))
echo "Steady start time: ${STEADY_START} ns"
echo "Steady end time: ${STEADY_END} ns"
STEADY_DUR=$(awk "BEGIN{printf \"%.1f\", $STEADY_END - $STEADY_START}")
echo "Steady duration: ${STEADY_DUR} ns"
echo "Steady responses: ${STEADY_CNT}"
awk -v cnt=$STEADY_CNT -v dur=$STEADY_DUR 'BEGIN{
  iops = cnt / (dur / 1e9)
  printf "Steady IOPS = %d / (%.1f / 1e9) s = %.4f M\n", cnt, dur, iops/1e6
}'

echo ""
echo "=== Prefetch DDR total ==="
# Total DDR reads = sum of all task total_reads
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | awk '{s+=$1;c++}END{printf "Total DDR reads from PTW tasks: %d (across %d tasks)\n",s,c}'

echo ""
echo "=== Average DDR per main task (WC hit) ==="
# For WC hit main tasks (total_reads=5 or 10)
grep 'DONE.*total_reads=' $LOG | grep -oP 'total_reads=\K\d+' | awk '
  $1==5{c5++} $1==10{c10++} $1==24{c24++}
  END{printf "5 DDR tasks: %d\n10 DDR tasks: %d\n24 DDR tasks: %d\n",c5,c10,c24}'
