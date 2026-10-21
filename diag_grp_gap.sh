#!/bin/bash
# 组内8数据任务注入间隔(修正: key=did+页基址)
set -u
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv
awk -F',' 'NR>1 && $1==3 && $6==0 {
  page = int($10/4096);            # iova>>12 = 4KB页基址
  key = $2 "_" page;
  n[key]++;
  # 记录该组前8个数据任务的 pop 时刻(按CSV到达顺序=注入顺序)
  if(n[key]<=8) pop[key "," n[key]] = $17;
}
END{
  total=0; sumgap=0; g500=0; g1k=0; g2k=0; g4k=0; gbig=0;
  sumspan=0; ngrp=0; spanmax=0; spanmin=99999999;
  for(key in n){
    if(n[key]==8){
      ngrp++;
      first=pop[key "," 1]; last=pop[key "," 8];
      span=last-first; sumspan+=span;
      if(span>spanmax) spanmax=span;
      if(span<spanmin) spanmin=span;
      for(i=1;i<8;i++){
        g=pop[key "," (i+1)]-pop[key "," i];
        total++; sumgap+=g;
        if(g<500) g500++; else if(g<1000) g1k++; else if(g<2000) g2k++; else if(g<4000) g4k++; else gbig++;
      }
    }
  }
  printf "完整8任务组数=%d\n", ngrp;
  printf "组内相邻任务间隔: avg=%.1f ns | <500ns=%d(%.1f%%) 500ns-1us=%d(%.1f%%) 1-2us=%d(%.1f%%) 2-4us=%d(%.1f%%) >4us=%d(%.1f%%)\n",
    sumgap/total, g500, 100.0*g500/total, g1k, 100.0*g1k/total, g2k, 100.0*g2k/total, g4k, 100.0*g4k/total, gbig, 100.0*gbig/total;
  printf "组首尾跨度(pop1->pop8): avg=%.1f ns min=%.1f max=%.1f ns\n", sumspan/ngrp, spanmin, spanmax;
  printf "校验: 14544组x8任务=%d\n", ngrp*8;
}' "$CSV"
