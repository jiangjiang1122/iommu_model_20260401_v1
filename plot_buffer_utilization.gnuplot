# Gnuplot script for Dedup Buffer Utilization (by Request ID)
set terminal png size 1200,600
set output 'dedup_buffer_utilization.png'
set title 'Dedup Buffer Utilization by Buffer Allocate Request ID'
set xlabel 'Buffer Allocate Request ID'
set ylabel 'Buffer Valid Count (entries)'
set yrange [0:512]
set grid
set datafile separator ','

# 只绘制allocate事件 (is_allocate=1)
plot 'dedup_buffer_utilization.csv' using ($5==1 ? ($.0+1) : 1/0):2 with lines title 'Buffer Valid Count' lw 1 lc rgb 'blue'
