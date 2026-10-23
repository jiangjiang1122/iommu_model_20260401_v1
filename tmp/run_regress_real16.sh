#!/usr/bin/env bash
# [16盘实测回放-零影响回归] 单设备(8通道) 512B 基线场景重跑:
#   复用 8盘回归时构建的 iommu_model_regress(BUILD_ROOT=build_regress, 时间戳 10-21 17:44,
#   早于 16盘改动, 未被重建/覆盖)。16盘改动=6个新文件+Makefile单分支插入, 不参与该二进制
#   构成, 因此重跑 IOPS 应与 8盘回归 post 值(287.02)逐位一致。
#   日志写 /tmp/real16_regress_post(不覆盖 8盘 /tmp/real8_regress_post)。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
RUNLOG=/tmp/real16_regress_post
R8LOG=/tmp/real8_regress_post/sim_post.log
rm -rf "$RUNLOG"; mkdir -p "$RUNLOG"
cd "$PROJ" || exit 1
echo "RUN_START $(date)"
timeout 2400 "$PROJ/iommu_model_regress" > "$RUNLOG/sim_post.log" 2>&1
echo "RUN_EXIT=$?"
echo "RUN_END $(date)"
echo "--- fatal ---"
grep -a -m5 -nE "FATAL|abort|Assertion|terminate|what\(\):" "$RUNLOG/sim_post.log" || echo none
echo "--- key markers ---"
grep -a -m1 "\[MC_CONFIG\]" "$RUNLOG/sim_post.log"
grep -a -m1 "\[MULTICHAN_RESULT\]" "$RUNLOG/sim_post.log"
grep -a -m1 "Steady IOPS" "$RUNLOG/sim_post.log"
grep -a -m1 "\[MD_FAIRNESS\]" "$RUNLOG/sim_post.log"
echo "--- 与8盘回归post日志逐位对账 ---"
if [ -f "$R8LOG" ]; then
  A=$(grep -a "Steady IOPS" "$R8LOG" | head -1)
  B=$(grep -a "Steady IOPS" "$RUNLOG/sim_post.log" | head -1)
  echo "real8_post: $A"
  echo "real16_rerun: $B"
  if [ "$A" = "$B" ]; then echo "BITWISE_MATCH=True (Steady IOPS 行逐位一致)"; else echo "BITWISE_MATCH=False"; fi
else
  echo "real8 post log 不存在, 仅与归档值 287.02 对账"
fi
echo "REAL16_REGRESSION_DONE"
