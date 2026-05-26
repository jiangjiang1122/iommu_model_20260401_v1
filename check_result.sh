#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
OUTPUT=${1:-output_slink_peq_test.txt}

echo "=== File: $OUTPUT ==="
wc -l $OUTPUT

echo ""
echo "=== Validation ==="
grep "Validation" $OUTPUT
grep "PASS" $OUTPUT

echo ""
echo "=== SLINK ==="
grep SLINK $OUTPUT

echo ""
echo "=== REORDER count ==="
grep -c '\[REORDER\] output' $OUTPUT

echo ""
echo "=== WRITE ORDER CHECK ==="
grep '\[REORDER\] output' $OUTPUT | grep 'WRITE' | awk -F'task_id=' '{print $2}' | awk -F',' '{print $1}' | awk 'BEGIN{prev=0;ok=1}{if($1<=prev){ok=0;print "ORDER ERROR: "$1" <= "prev};prev=$1}END{if(ok)print "WRITE ORDER: OK (monotonic)"}'

echo ""
echo "=== DDR Peak Outstanding ==="
grep '\[DDR_DISPATCH\]' $OUTPUT | awk -F'rd_out=' '{print $2}' | awk -F', wr_out=' '{rd=$1; wr=$2; if(rd>maxrd)maxrd=rd; if(wr>maxwr)maxwr=wr}END{print "Peak READ: "maxrd", WRITE: "maxwr}'

echo ""
echo "=== AXI Master Peak ==="
grep 'axi_master_0_out=' $OUTPUT | awk -F'axi_master_0_out=' '{print $2}' | awk '{if($1>max)max=$1}END{print "Peak axi_master_0: "max}'
grep 'axi_master_1 outstanding' $OUTPUT | awk -F'-> ' '{print $2}' | awk '{v=$1+0; if(v>max)max=v}END{print "Peak axi_master_1: "max}'

echo ""
echo "=== Time range ==="
grep -o '\[t=[0-9]* ns\]' $OUTPUT | tail -3

echo ""
echo "=== Cache Stats ==="
grep -E "dc_cache|walker_ptw_c3" $OUTPUT | grep -v "CONVERT\|COLLECTOR\|LOOKUP\|UPDATE\|DC_SIZE" | tail -5
