#!/bin/bash
for f in test_100pkts.log test_1000pkts.log test_2000pkts_twostage.log test_5000pkts.log; do
    echo "=== $f ==="
    grep -E "IOPS|Completed|total tasks|avg DDR|Overall|steady|Throughput|PTW  C|PTW  I|PTW  a|PTW  M" "$1/$f" | grep -v SKIP | head -25
    echo
done
