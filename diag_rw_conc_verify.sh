#!/bin/bash
# 验证: 逐时刻重构读写并发(事件法), 统计写并发各档位下读并发的表现
set -u
export LC_ALL=C
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv
OUT=/tmp/task2_rw_conc.txt
: > "$OUT"

# 事件流: admit(+1) release(-1), 分读写
awk -F',' 'NR>1 && $1==3 {
  w=$7; t=$18; printf "%.1f %d %d\n", t, w, 1;
  t2=$22; printf "%.1f %d %d\n", t2, w, -1;
}' "$CSV" | sort -n > /tmp/rw_events.txt

# 前缀和扫描: 输出时段 [t_prev,t) 的 R/W 并发
awk '{
  t=$1; w=$2; d=$3;
  if(NR>1 && t>t_prev){
    dt = t - t_prev;
    # 按写并发分档累计读并发时间加权和时长
    bucket = (wc>=320) ? 0 : (wc>=300) ? 1 : (wc>=250) ? 2 : (wc>=200) ? 3 : (wc>=150) ? 4 : 5;
    rsum[bucket] += rc*dt; wsum[bucket] += wc*dt; tsum[bucket] += dt;
    rmax = (rc>rmax)?rc:rmax; wmax=(wc>wmax)?wc:wmax;
    if(wc>=320){ rwfull += rc*dt; twfull += dt; if(rc>rmaxwf) rmaxwf=rc; }
  }
  t_prev = t;
  if(w==0) rc+=d; else wc+=d;
}
END{
  split("W>=320(顶满) W300-319 W250-299 W200-249 W150-199 W<150", names, " ");
  printf "%-16s %10s %12s %12s\n", "写并发档位", "时长ns", "读并发avg", "写并发avg";
  for(b=0;b<=5;b++){
    if(tsum[b]>0) printf "%-16s %10.0f %12.1f %12.1f\n", names[b+1], tsum[b], rsum[b]/tsum[b], wsum[b]/tsum[b];
  }
  printf "\n写顶满时段: 时长=%.0fns(占%.1f%%), 期间读并发avg=%.1f, 读并发max=%.0f\n", twfull, 100*twfull/t_prev, rwfull/twfull, rmaxwf;
  printf "全程: 读并发max=%.0f 写并发max=%.0f 窗口=%.0fns\n", rmax, wmax, t_prev;
}' /tmp/rw_events.txt >> "$OUT"

# 读/写到达间隔(admit口径)
echo "" >> "$OUT"
echo "===== 读/写任务 admit 到达间隔 (活跃跨度内) =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 {print $7, $18}' "$CSV" | sort -k2 -n | awk '
$1==0{if(pr>0){d=$2-pr; rs+=d; rn++; if(d<rmin||rmin==0)rmin=d; if(d>rmax)rmax=d;} pr=$2; if(first_r==0)first_r=$2; last_r=$2}
$1==1{if(pw>0){d=$2-pw; ws+=d; wn++; if(d>wmin||wmin==0)wmin=d; if(d>wmax)wmax=d;} pw=$2; if(first_w==0)first_w=$2; last_w=$2}
END{
  printf "READ : n=%d 跨度=%.0fns 到达间隔avg=%.2fns min=%.1f max=%.1f\n", rn, last_r-first_r, rs/rn, rmin, rmax;
  printf "WRITE: n=%d 跨度=%.0fns 到达间隔avg=%.2fns min=%.1f max=%.1f\n", wn, last_w-first_w, ws/wn, wmin, wmax;
  printf "READ 线速参考: 平均包%.0fB -> 64GB/s线速间隔=%.2fns\n", (58368*512+14544*32)/72912, (58368*512+14544*32)/72912/64.0;
  printf "WRITE线速参考: 平均包%.0fB -> 64GB/s线速间隔=%.2fns\n", (57984*512+14544*16+14560*4)/87288, (57984*512+14544*16+14560*4)/87288/64.0;
}' >> "$OUT"

echo RW_CONC_DONE >> "$OUT"
cat "$OUT"
