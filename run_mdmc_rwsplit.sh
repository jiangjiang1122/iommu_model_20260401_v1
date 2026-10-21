#!/bin/bash
# 运行 16设备×8通道 multidev_mc 读写端口分离(SPLIT=1)二进制, 重定向大日志到 /tmp,
#   跑完立即抽取关键校验标记(不打印整份 600MB 日志)。
set -u
PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
cd "$PROJ" || exit 1
BIN=/tmp/iommu_model_mdmc_rwsplit_n16c8
LOG=/tmp/mdmc_rwsplit_n16c8.log
echo "RUN_START $(date)"
"$BIN" > "$LOG" 2>&1
EX=$?
echo "RUN_EXIT=$EX"
echo "RUN_END $(date)  log_size=$(du -h "$LOG" | cut -f1)  log_lines=$(wc -l < "$LOG")"
echo "===== FATAL / abort / require (if any) ====="
grep -nE "FATAL|abort|Assertion|terminate|what\(\):|失败|异常" "$LOG" | head -30
echo "===== KEY MARKERS ====="
grep -m1 "\[MDMC_CONFIG\]" "$LOG"
grep -m1 "\[MDMC_UNIQUE_OK\]" "$LOG"
grep -m1 "\[MDMC_INJECT_END\]" "$LOG"
grep -m1 "\[MDMC_RWQ\]" "$LOG"
grep -m1 "\[MDMC_SUPPLY\]" "$LOG"
grep -m1 "\[MULTIDEV_MC_RESULT\]" "$LOG"
echo "===== direction mismatch (must be 0) ====="
grep -nE "方向校验失配|IOMMU_PORT_DIR|R-PORT|W-PORT" "$LOG" | head -8
echo "===== Jain / fairness ====="
grep -niE "jain|fairness" "$LOG" | head -10
echo "===== CONC_WIN ====="
grep -nE "\[CONC_WIN\]" "$LOG" | head -6
echo "===== CSV outputs (rwsplit prefix) ====="
ls -la "$PROJ"/multidev_mc_rwsplit_n16_c8_512mb_* 2>/dev/null | head
echo "RUN_SCRIPT_DONE"
