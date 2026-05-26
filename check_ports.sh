#!/bin/bash
echo "=== axi_master_0 (PCIe NoC) peak outstanding ==="
grep -oP 'axi_master_0_out=\d+' output_2000req.txt | sed 's/axi_master_0_out=//' | sort -n | tail -1

echo "=== axi_master_1 (DDR CMN) peak outstanding ==="
grep -oP 'axi_master_1 outstanding\+\+ -> \d+' output_2000req.txt | grep -oP '\d+$' | sort -n | tail -1

echo "=== Total axi_master_0 sends ==="
grep -c 'axi_master_0_out=' output_2000req.txt

echo "=== Total axi_master_1 sends ==="
grep -c 'axi_master_1 outstanding++' output_2000req.txt
