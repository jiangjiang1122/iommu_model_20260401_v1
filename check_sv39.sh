#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
OUTPUT=output_sv39_slink_test.txt

echo "=== Validation ==="
grep "Validation" $OUTPUT
grep "PASS" $OUTPUT

echo "=== REORDER count ==="
grep -c '\[REORDER\] output' $OUTPUT

echo "=== WRITE ORDER CHECK ==="
grep '\[REORDER\] output' $OUTPUT | grep 'WRITE' | awk -F'task_id=' '{print $2}' | awk -F',' '{print $1}' | awk 'BEGIN{prev=0;ok=1}{if($1<=prev){ok=0;print "ORDER ERROR: "$1" <= "prev};prev=$1}END{if(ok)print "WRITE ORDER: OK (monotonic)"}'
