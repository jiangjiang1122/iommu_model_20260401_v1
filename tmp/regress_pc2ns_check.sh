#!/usr/bin/env bash
# [16盘回归补充验证] 证明 287.02(8盘post) vs 286.90(本次重跑) 的 0.04% 差异仅由
# default_config.json PC 段(2ns->1ns, 8盘回归之后的既有配置改动)引起:
#   1) 隔离目录放 HEAD 版配置(PC=2ns) 重跑同一 iommu_model_regress 二进制,
#      应与 8盘 post 287.02 逐位一致 -> 证明 16盘改动零影响;
#   2) PC 配置打印行 diff 作为佐证。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
RUNLOG=/tmp/real16_regress_pc2ns
rm -rf "$RUNLOG"; mkdir -p "$RUNLOG/iommu/cache_config"
git -C "$PROJ" show HEAD:iommu/cache_config/default_config.json > "$RUNLOG/iommu/cache_config/default_config.json" || exit 1
cd "$RUNLOG" || exit 1
echo "RUN_START $(date)"
timeout 2400 "$PROJ/iommu_model_regress" > "$RUNLOG/sim_pc2ns.log" 2>&1
echo "RUN_EXIT=$?"
echo "--- key markers ---"
grep -a -m1 "\[MULTICHAN_RESULT\]" "$RUNLOG/sim_pc2ns.log"
grep -a -m1 "Steady IOPS" "$RUNLOG/sim_pc2ns.log"
grep -a -m1 "\[MD_FAIRNESS\]" "$RUNLOG/sim_pc2ns.log"
echo "--- 对账 ---"
A=$(grep -a "Steady IOPS" /tmp/real8_regress_post/sim_post.log | head -1)
B=$(grep -a "Steady IOPS" "$RUNLOG/sim_pc2ns.log" | head -1)
echo "real8_post_1021: $A"
echo "real16_rerun_pc2ns: $B"
[ "$A" = "$B" ] && echo "BITWISE_MATCH=True (PC=2ns口径下逐位一致 -> 16盘改动零影响)" || echo "BITWISE_MATCH=False"
echo "--- 当前工作区配置(PC=1ns)下重跑值(与用户锚点对账) ---"
grep -a "Steady IOPS" /tmp/real16_regress_post/sim_post.log | head -1
echo "PC2NS_CHECK_DONE"
