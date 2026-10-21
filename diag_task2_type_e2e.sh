#!/bin/bash
# Q3/Q5 补充: 按任务类型(数据/SQ/CQ/MSI)统计 e2e; Q4: PT Cache 配置
set -u
export LC_ALL=C
OUT=/tmp/task2_type_e2e.txt
: > "$OUT"
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv

echo "===== [1] 按type分类: 数量/字节/e2e(pop->response) avg/max/p90 =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 {
  t=$6; w=$7; b=$8; e2e=$21-$17;
  cnt[t]++; bytes[t]+=b; sum[t]+=e2e;
  if(e2e>mx[t]) mx[t]=e2e;
  key=t"_"w; cntw[key]++; sumw[key]+=e2e;
  all++; allsum+=e2e; if(e2e>allmx) allmx=e2e;
  # 数据任务 e2e 分布桶
  if(t==0){
    if(e2e<1000) b1++;
    else if(e2e<2000) b2++;
    else if(e2e<3000) b3++;
    else if(e2e<5000) b4++;
    else b5++;
  }
}
END{
  printf "ALL(epoch3): n=%d avg=%.1f max=%.1f\n", all, allsum/all, allmx;
  split("0 DATA 1 SQ 2 CQ 3 MSI", names, " ");
  for(t=0;t<=3;t++){
    if(cnt[t]>0) printf "type=%d(%s): n=%d bytes_avg=%.0f e2e_avg=%.1f e2e_max=%.1f\n", t, names[t*2+2], cnt[t], bytes[t]/cnt[t], sum[t]/cnt[t], mx[t];
  }
  printf "DATA e2e分布: <1us=%d 1-2us=%d 2-3us=%d 3-5us=%d >5us=%d\n", b1,b2,b3,b4,b5;
  # 数据读写细分
  for(k in cntw){ split(k,a,"_"); printf "type=%d write=%d: n=%d e2e_avg=%.1f\n", a[1],a[2],cntw[k],sumw[k]/cntw[k]; }
}' "$CSV" >> "$OUT"

echo "" >> "$OUT"
echo "===== [2] 数据任务(512B) e2e 分位数 =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 && $6==0 {print $21-$17}' "$CSV" | sort -n > /tmp/e2e_data.txt
n=$(wc -l < /tmp/e2e_data.txt)
for p in 50 90 99; do
  idx=$(( n*p/100 + 1 ))
  v=$(sed -n "${idx}p" /tmp/e2e_data.txt)
  printf "DATA p%d = %s ns (n=%d)\n" "$p" "$v" "$n" >> "$OUT"
done

echo "" >> "$OUT"
echo "===== [3] 控制任务 e2e 分位数 =====" >> "$OUT"
awk -F',' 'NR>1 && $1==3 && $6>0 {print $21-$17}' "$CSV" | sort -n > /tmp/e2e_ctrl.txt
n2=$(wc -l < /tmp/e2e_ctrl.txt)
for p in 50 90 99; do
  idx=$(( n2*p/100 + 1 ))
  v=$(sed -n "${idx}p" /tmp/e2e_ctrl.txt)
  printf "CTRL p%d = %s ns (n=%d)\n" "$p" "$v" "$n2" >> "$OUT"
done

echo "" >> "$OUT"
echo "===== [4] PT Cache / dedup 几何配置 =====" >> "$OUT"
grep -m 6 -E 'PT Cache|PT_CACHE|pt_cache.*sets|Multi-RAM|Walker Cache.*Config' /tmp/mdmc_rwsplit_n16c8.log | head -8 >> "$OUT"
echo "--- default_config.json pt_cache/dedup 段 ---" >> "$OUT"
python3 - <<'PYEOF' >> "$OUT" 2>&1
import json
with open('/mnt/d/Qoder_proj/iommu_model_20260401_v1/iommu/cache_src/common/default_config.json') as f:
    cfg = json.load(f)
for k in ('pt_cache','dedup_cache'):
    if k in cfg:
        print(k, '=', json.dumps(cfg[k]))
PYEOF

echo TYPE_E2E_DONE >> "$OUT"
