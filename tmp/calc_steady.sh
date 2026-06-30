#!/bin/bash
LOG=/mnt/d/Qoder_proj/iommu_model_20260401_v1/sim4_output.log

RESP_COUNT=$(grep -c 'send_response_to_initiator' $LOG)
echo "Total responses: $RESP_COUNT"

# Steady state: 10% to 90%
START_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.1}")
END_CNT=$(awk "BEGIN{printf \"%d\", $RESP_COUNT * 0.9}")
STEADY_CNT=$((END_CNT - START_CNT))
echo "Steady state range: response #${START_CNT} to #${END_CNT} (${STEADY_CNT} responses)"

STEADY_START=$(grep 'send_response_to_initiator' $LOG | sed -n "${START_CNT}p" | grep -oP 'entry=\K[0-9.]+')
STEADY_END=$(grep 'send_response_to_initiator' $LOG | sed -n "${END_CNT}p" | grep -oP 'exit=\K[0-9.]+')
echo "Steady start time: ${STEADY_START} ns"
echo "Steady end time: ${STEADY_END} ns"
STEADY_DUR=$(awk "BEGIN{printf \"%.1f\", $STEADY_END - $STEADY_START}")
echo "Steady duration: ${STEADY_DUR} ns"
awk -v cnt=$STEADY_CNT -v dur=$STEADY_DUR 'BEGIN{
  iops = cnt / (dur / 1e9)
  printf "Steady IOPS = %d / (%.1f / 1e9) s = %.4f M\n", cnt, dur, iops/1e6
}'

# Also calculate steady state average latency
echo ""
echo "=== Steady state avg latency ==="
grep 'send_response_to_initiator' $LOG | sed -n "${START_CNT},${END_CNT}p" | grep -oP 'e2e=\K[0-9.]+' | awk '{s+=$1;c++}END{printf "Avg E2E (steady): %.1f ns (%d samples)\n",s/c,c}'
