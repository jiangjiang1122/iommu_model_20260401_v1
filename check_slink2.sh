#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
OUTPUT=output_slink_test2.txt

echo "=== Total lines ==="
wc -l $OUTPUT

echo ""
echo "=== PASS/FAIL lines ==="
grep -n "PASS\|FAIL\|pass\|fail\|Pass\|Fail" $OUTPUT | head -10

echo ""
echo "=== Total REORDER outputs ==="
grep -c '\[REORDER\] output' $OUTPUT

echo ""
echo "=== Total RP requests sent ==="
grep -c 'send_translation_request' $OUTPUT

echo ""
echo "=== Completed tasks ==="
grep -c 'COMPLETED\|completed' $OUTPUT

echo ""  
echo "=== Last 20 lines ==="
tail -20 $OUTPUT

echo ""
echo "=== Time range ==="
grep -o '\[t=[0-9]* ns\]' $OUTPUT | tail -5
grep 'resp_time=' $OUTPUT | tail -3
