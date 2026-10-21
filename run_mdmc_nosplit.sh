#!/bin/bash
# 运行 SPLIT=0 回退分支二进制, 确认 #else(单端口直发)路径跑通 PASS, 并回读稳态 IOPS 做回归对照。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1
BIN=/tmp/iommu_model_mdmc_nosplit_n16c8
LOG=/tmp/mdmc_nosplit_n16c8.log
echo "RUN_START $(date)"
"$BIN" > "$LOG" 2>&1
echo "RUN_EXIT=$?"
echo "RUN_END $(date)  log_size=$(du -h "$LOG" | cut -f1)"
echo "===== FATAL / abort (if any) ====="
grep -aE "FATAL|abort|Assertion|terminate|what\(\):" "$LOG" | head -20
echo "===== KEY MARKERS ====="
grep -am1 "\[MDMC_CONFIG\]" "$LOG"
grep -am1 "\[MDMC_UNIQUE_OK\]" "$LOG"
grep -am1 "\[MULTIDEV_MC_RESULT\]" "$LOG"
echo "===== steady IOPS / fairness ====="
grep -aE "\[MD_FAIRNESS\]" "$LOG" | head
echo "===== single-port callback in use (non-SPLIT) ====="
grep -am2 "axi_slave_nb_transport_fw" "$LOG"
echo "===== report prefix CSVs (baseline, non-rwsplit) ====="
ls -la "$PROJ"/multidev_mc_n16_c8_512mb_summary.csv 2>/dev/null | head
echo "RUN_NOSPLIT_DONE"
