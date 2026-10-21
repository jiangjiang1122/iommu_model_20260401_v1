#!/bin/bash
# 基线A(multichan n=8)组内数据任务注入间隔 + 类型e2e
set -u
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multichan_rwsplit_n8_512mb_requests.csv
echo "--- 行数与epoch ---"
wc -l "$CSV"
awk -F',' 'NR>1{e[$1]++} END{for(k in e) printf "epoch=%s n=%d\n", k, e[k]}' "$CSV"
echo "--- 组内8数据任务间隔 (key=did+页基址) ---"
awk -F',' 'NR>1 && $6==0 {
  page = int($10/4096); key = $2 "_" page; n[key]++;
  if(n[key]<=8) pop[key "," n[key]] = $17;
}
END{
  total=0; sumgap=0; g100=0; g300=0; g1k=0; g4k=0; gbig=0;
  ngrp=0; sumspan=0;
  for(key in n){
    if(n[key]==8){
      ngrp++; sumspan += pop[key "," 8]-pop[key "," 1];
      for(i=1;i<8;i++){
        g=pop[key "," (i+1)]-pop[key "," i];
        total++; sumgap+=g;
        if(g<100) g100++; else if(g<300) g300++; else if(g<1000) g1k++; else if(g<4000) g4k++; else gbig++;
      }
    }
  }
  printf "完整8任务组数=%d\n", ngrp;
  printf "组内相邻任务间隔: avg=%.1f ns | <100ns=%d(%.1f%%) 100-300ns=%d(%.1f%%) 300ns-1us=%d(%.1f%%) 1-4us=%d(%.1f%%) >4us=%d(%.1f%%)\n",
    sumgap/total, g100,100.0*g100/total, g300,100.0*g300/total, g1k,100.0*g1k/total, g4k,100.0*g4k/total, gbig,100.0*gbig/total;
  printf "组首尾跨度(pop1->pop8): avg=%.1f ns\n", sumspan/ngrp;
}' "$CSV"
echo "--- 按type e2e ---"
awk -F',' 'NR>1 {t=$6; cnt[t]++; sum[t]+=$21-$17} END{for(t=0;t<=3;t++) if(cnt[t]>0) printf "type=%d: n=%d e2e_avg=%.1f\n", t, cnt[t], sum[t]/cnt[t]}' "$CSV"
