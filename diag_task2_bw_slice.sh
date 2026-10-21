#!/bin/bash
# Q1补充: 端口带宽50us时间切片(读口/写口分通道); Q4补充: 组内数据任务注入间隔
set -u
export LC_ALL=C
OUT=/tmp/task2_bw_slice.txt
: > "$OUT"
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv

echo "===== [1] 端口带宽 50us 时间切片 (按 accept_ns 分桶, 读口=write0, 写口=write1) =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 {
  b=int($16/50000);            # accept_ns 50us 桶
  if($7==0) rb[b]+=$8; else wb[b]+=$8;
  if($16>maxb) maxb=$16;
}
END{
  printf "%-10s %-22s %-22s\n", "bucket_us", "ReadPort GB/s", "WritePort GB/s";
  for(b=0;b<=int(maxb/50000);b++){
    r=rb[b]/50000.0; w=wb[b]/50000.0;   # B/ns = GB/s
    printf "%-10s %-22.3f %-22.3f\n", sprintf("[%d,%d)", b*50, b*50+50), r, w;
  }
}' "$CSV" >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] 组内8个数据任务注入间隔 (同 did+group, 按 pop_ns 排序) =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 && $6==0 {
  key=$2"_"$5; n[key]++; pop[key","n[key]]=$17;
  if(n[key]>maxn[key]) maxn[key]=n[key];
}
END{
  # 组内相邻间隔分布 + 组跨度
  total=0; sumgap=0; g500=0; g1k=0; g2k=0; g4k=0; gbig=0;
  sumspan=0; ngrp=0; spanmax=0;
  for(key in n){
    if(n[key]==8){
      ngrp++; first=pop[key","1]; last=pop[key","8];
      span=last-first; sumspan+=span; if(span>spanmax) spanmax=span;
      for(i=1;i<8;i++){
        g=pop[key","i+1]-pop[key","i];
        total++; sumgap+=g;
        if(g<500) g500++; else if(g<1000) g1k++; else if(g<2000) g2k++; else if(g<4000) g4k++; else gbig++;
      }
    }
  }
  printf "完整8任务组数=%d\n", ngrp;
  printf "组内相邻任务间隔: avg=%.1f ns | <500ns=%d(%.1f%%) 500-1us=%d 1-2us=%d 2-4us=%d >4us=%d\n", sumgap/total, g500, 100.0*g500/total, g1k, g2k, g4k, gbig;
  printf "组首尾跨度(pop1->pop8): avg=%.1f ns max=%.1f ns\n", sumspan/ngrp, spanmax;
}' "$CSV" >> "$OUT"

echo BW_SLICE_DONE >> "$OUT"
