#!/bin/bash
# Check write ordering
OUTPUT=${1:-output_2000req.txt}
echo "=== Analyzing: $OUTPUT ==="

grep 'REORDER.*output.*WRITE' "$OUTPUT" | sed 's/.*task_id=//;s/,.*//' | awk '
BEGIN{ok=1;prev=0}
{if($1!=prev+1){ok=0;print "ORDER BREAK at "$1" prev="prev} prev=$1}
END{if(ok) print "WRITE ORDER: PASS (1->"prev" strictly monotonic)"; else print "WRITE ORDER: FAIL"}'

echo ""
echo "=== DDR Concurrent Statistics ==="
echo -n "Max READ outstanding: "
grep 'DDR_DISPATCH' "$OUTPUT" | grep -oP 'rd_out=\d+' | sed 's/rd_out=//' | sort -n | tail -1
echo -n "Max WRITE outstanding: "
grep 'DDR_DISPATCH' "$OUTPUT" | grep -oP 'wr_out=\d+' | sed 's/wr_out=//' | sort -n | tail -1
echo -n "Total DDR transactions: "
grep -c 'DDR_DISPATCH' "$OUTPUT"

echo ""
echo "=== AXI Master Port Outstanding ==="
echo -n "Max axi_master_0 (PCIe NoC): "
grep -oP 'axi_master_0_out=\d+' "$OUTPUT" | sed 's/axi_master_0_out=//' | sort -n | tail -1
echo -n "Max axi_master_1 (DDR CMN): "
grep -oP 'axi_master_1 outstanding\+\+ -> \d+' "$OUTPUT" | grep -oP '\d+$' | sort -n | tail -1

echo ""
echo "=== Cache Hit Rate Summary ==="
grep -A15 'IOMMU Cache Statistics Summary' "$OUTPUT" | grep -E 'dc_cache|pt_cache|walker_cache|walker_ptw'
