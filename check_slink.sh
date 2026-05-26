#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
OUTPUT=output_slink_test2.txt

echo "=== PASS/FAIL ==="
PASS_CNT=$(grep -c "PASS" $OUTPUT)
FAIL_CNT=$(grep -c "FAIL" $OUTPUT)
echo "PASS: $PASS_CNT"
echo "FAIL: $FAIL_CNT"

echo ""
echo "=== SLINK ==="
grep SLINK $OUTPUT

echo ""
echo "=== WRITE ORDER (first 5) ==="
grep '\[REORDER\] output' $OUTPUT | grep 'WRITE' | head -5

echo ""
echo "=== WRITE ORDER CHECK ==="
grep '\[REORDER\] output' $OUTPUT | grep 'WRITE' | awk -F'task_id=' '{print $2}' | awk -F',' '{print $1}' | awk 'BEGIN{prev=0;ok=1}{if($1<=prev){ok=0;print "ORDER ERROR: "$1" <= "prev};prev=$1}END{if(ok)print "WRITE ORDER: OK (monotonic)"}'

echo ""
echo "=== DDR Peak Outstanding ==="
grep '\[DDR_DISPATCH\]' $OUTPUT | awk -F'rd_out=' '{print $2}' | awk -F', wr_out=' '{rd=$1; wr=$2; if(rd>maxrd)maxrd=rd; if(wr>maxwr)maxwr=wr}END{print "Peak READ outstanding: "maxrd; print "Peak WRITE outstanding: "maxwr}'

echo ""
echo "=== AXI Master Peak ==="
grep 'axi_master_0_out=' $OUTPUT | awk -F'axi_master_0_out=' '{print $2}' | awk '{if($1>max)max=$1}END{print "Peak axi_master_0: "max}'
grep 'axi_master_1 outstanding' $OUTPUT | awk -F'-> ' '{print $2}' | awk '{v=$1+0; if(v>max)max=v}END{print "Peak axi_master_1: "max}'

echo ""
echo "=== CACHE HIT RATES ==="
grep -E "Total Accesses|Hit Rate" $OUTPUT | head -20
