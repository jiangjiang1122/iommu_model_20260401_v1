#!/bin/bash
# 诊断: group 列语义 (did=0 前 45 个任务的 seq/group/type/write/bytes)
set -u
CSV=/mnt/d/Qoder_proj/iommu_model_20260401_v1/multidev_mc_rwsplit_n16_c8_512mb_requests.csv
awk -F',' 'NR>1 && $1==3 && $2==0 && $4<45 {printf "seq=%s group=%s type=%s w=%s bytes=%s\n", $4, $5, $6, $7, $8}' "$CSV" | head -45
echo "--- key计数分布 (did_group, type=0) ---"
awk -F',' 'NR>1 && $1==3 && $6==0 {n[$2"_"$5]++} END{for(k in n) c[n[k]]++; for(v in c) printf "n=%d 的组数=%d\n", v, c[v]}' "$CSV"
