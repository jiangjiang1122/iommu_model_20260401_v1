#!/usr/bin/env bash
# [16盘核验] 实证: 同原始页在两组映射为2个不同4KB页 / 组间页交集 / GSCID集合
set -eu
A=/mnt/d/Qoder_proj/iommu_model_20260401_v1/rwsplit_results/real16_trace_20261022_173131
echo "== remap 列: dir,group,excel_row,did,bytes,is_ctrl,source_page_no,offset,gen_id,mapped_page_no,gpa_page_base,expected_spa =="
head -1 "$A/real16_trace_remap.csv"
echo "== 读方向前4行(应为 A0,B0,A1,B1 逐包交替) =="
grep "^R," "$A/real16_trace_remap.csv" | head -4
echo "== 写方向前4行 =="
grep "^W," "$A/real16_trace_remap.csv" | head -4
echo "== 同原始页(source_page_no相同)在两组的映射行对比 =="
P=$(grep "^R," "$A/real16_trace_remap.csv" | head -1 | cut -d, -f7)
echo "-- 读方向 source_page_no=$P 的全部行:"
awk -F, -v p="$P" '$1=="R" && $7==p' "$A/real16_trace_remap.csv"
PW=$(grep "^W," "$A/real16_trace_remap.csv" | head -1 | cut -d, -f7)
echo "-- 写方向 source_page_no=$PW 的全部行:"
awk -F, -v p="$PW" '$1=="W" && $7==p' "$A/real16_trace_remap.csv"
echo "== 读方向组A/组B不同mapped_page数与交集 =="
awk -F, '$1=="R"{if($2==0)a[$10]=1;else b[$10]=1} END{na=0;for(k in a)na++;nb=0;for(k in b)nb++;inter=0;for(k in a)if(k in b)inter++;printf "A_pages=%d B_pages=%d intersect=%d\n",na,nb,inter}' "$A/real16_trace_remap.csv"
echo "== 写方向组A/组B不同mapped_page数与交集 =="
awk -F, '$1=="W"{if($2==0)a[$10]=1;else b[$10]=1} END{na=0;for(k in a)na++;nb=0;for(k in b)nb++;inter=0;for(k in a)if(k in b)inter++;printf "A_pages=%d B_pages=%d intersect=%d\n",na,nb,inter}' "$A/real16_trace_remap.csv"
echo "== gscid 实际使用集合(requests.csv 第3列) =="
awk -F, 'NR>1{gs[$3]=1} END{n=0;for(k in gs){printf "%s ",k;n++}printf "(共%d个)\n",n}' "$A/real16_trace_requests.csv"
echo "== did 使用集合 =="
awk -F, 'NR>1{ds[$2]=1} END{n=0;for(k in ds){printf "%s ",k;n++}printf "(共%d个)\n",n}' "$A/real16_trace_requests.csv"
echo "VERIFY_DONE"
