#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
echo "=== PTW Task DDR Read Distribution ==="
grep 'PTW_STAT.*two_stage_pf_complete' sim_5000_twostage.log | grep -oP 'DDR_reads=\d+' | sed 's/DDR_reads=//g' | sort | uniq -c
echo ""
echo "=== Walker Cache Hit Distribution ==="
grep 'PTW_STAT.*two_stage_pf_complete' sim_5000_twostage.log | grep -oP 'Walker_HIT=\d+' | sed 's/Walker_HIT=//g' | sort | uniq -c
echo ""
echo "=== Walker Cache Hit Level Distribution ==="
grep 'PTW_STAT.*two_stage_pf_complete.*Walker_HIT=1' sim_5000_twostage.log | grep -oP 'hit_level=\d+' | sed 's/hit_level=//g' | sort | uniq -c
