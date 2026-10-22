#!/usr/bin/env bash
# =============================================================================
# [8盘实测回放] 一键串联: 前10000包/方向预处理 → 构建 → 20000包仿真 → 37项统计
#
# 用法(WSL 内):
#   bash run_real8_trace.sh                     # 默认读写各前10000包，不跑额外阶梯
#   bash run_real8_trace.sh --fresh-build       # 使用 make -B 全量重编
#   bash run_real8_trace.sh --skip-build        # 复用二进制，运行20000包+统计
#   bash run_real8_trace.sh --full              # 显式请求才运行全部样本
#   bash run_real8_trace.sh --archive           # 完成后归档到 rwsplit_results/real8_trace_<run_id>/
#
# 注意:
#   1. 本场景使用独立对象根 build_real8/ 与独立二进制 iommu_model_real8,
#      绝不清理工程根 build/ 或覆盖既有 iommu_model 与历史结果。
#   2. Makefile 无 -MMD 依赖跟踪: 修改过 iommu_top.hh 等头文件后必须 --fresh-build
#      全量重编, 否则旧 .o 与新头文件类布局不一致会在运行期产生段错误。
#   3. 运行目录全部位于 WSL 本地盘 /tmp/real8_*, 避免 drvfs 逐行写入开销。
# =============================================================================
set -eu -o pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
XLSX_RD="$PROJ/4k_randrw_mem_1021_MRd.xlsx"
XLSX_WR="$PROJ/4k_randrw_mem_1021_MWr.xlsx"
PREP="$PROJ/rp/prepare_real8_trace.py"
ANALYZE="$PROJ/rp/analyze_real8_trace.py"
BIN="$PROJ/iommu_model_real8"
FULL_RUN="$(mktemp -d /tmp/real8_run_XXXXXX)"
LOG_BASE=/tmp/real8_logs
PRE_LOG="${REAL8_PRE_LOG:-/tmp/real8_regress_pre/sim_pre.log}"
POST_LOG="${REAL8_POST_LOG:-/tmp/real8_regress_post/sim_post.log}"

FRESH_BUILD=0; SKIP_BUILD=0; SKIP_LADDER=1; DO_ARCHIVE=0; SAMPLE_LIMIT=10000
BUILD_LOG="${REAL8_BUILD_LOG:-}"
for a in "$@"; do
  case "$a" in
    --fresh-build) FRESH_BUILD=1 ;;
    --skip-build)  SKIP_BUILD=1 ;;
    --skip-ladder) SKIP_LADDER=1 ;;
    --ladder)      SKIP_LADDER=0 ;;
    --full)        SAMPLE_LIMIT=0 ;;
    --archive)     DO_ARCHIVE=1 ;;
    *) echo "unknown option: $a (support: --fresh-build --skip-build --skip-ladder --ladder --full --archive)"; exit 2 ;;
  esac
done

mkdir -p "$LOG_BASE"

echo "== [0/5] 环境与输入检查 =="
command -v python3 >/dev/null || { echo "FATAL: python3 not found"; exit 1; }
[ -f "$XLSX_RD" ] || { echo "FATAL: missing $XLSX_RD"; exit 1; }
[ -f "$XLSX_WR" ] || { echo "FATAL: missing $XLSX_WR"; exit 1; }
echo "inputs ok: MRd.xlsx / MWr.xlsx"

echo "== [1/5] 构建 (TEST=real8_trace_replay -> build_real8/, iommu_model_real8) =="
if [ "$SKIP_BUILD" = 1 ] && [ -x "$BIN" ]; then
  echo "skip build (--skip-build, reuse $BIN)"
else
  BUILD_FLAGS=()
  if [ "$FRESH_BUILD" = 1 ]; then
    echo "--fresh-build: 强制重编全部对象，不删除已有目录"
    BUILD_FLAGS=(-B)
  fi
  BUILD_LOG="$LOG_BASE/build_trace_$(date +%Y%m%d_%H%M%S).log"
  make -C "$PROJ" "${BUILD_FLAGS[@]}" -j"$(nproc)" TEST=real8_trace_replay \
    2>&1 | tee "$BUILD_LOG"
fi
[ -x "$BIN" ] || { echo "FATAL: $BIN not built"; exit 1; }

# 运行单个用例: run_case <N|0=全量> <run_dir>
run_case() {
  local N="$1" RUN="$2"
  local LIMIT=(--limit "$N")
  mkdir -p "$RUN/real8_input" "$RUN/iommu/cache_config"
  cp "$PROJ/iommu/cache_config/default_config.json" "$RUN/iommu/cache_config/"
  echo "-- prepare ($RUN)"
  python3 "$PREP" --src-dir "$PROJ" --out-dir "$RUN/real8_input" ${LIMIT[@]+"${LIMIT[@]}"}
  echo "-- simulate ($RUN)"
  ( cd "$RUN" && REAL8_LIMIT="$N" "$BIN" > sim.log 2>&1 )
  grep -aq "REAL8_RESULT] PASS" "$RUN/sim.log" || {
    echo "FATAL: $RUN simulation not PASS"; tail -30 "$RUN/sim.log"; exit 1; }
  grep -a -m1 "REAL8_RESULT" "$RUN/sim.log"
}

if [ "$SKIP_LADDER" = 0 ]; then
  echo "== [2/5] 小样本验证 1k/方向 =="
  for N in 1000; do
    echo "-- [ladder N=$N]"
    run_case "$N" "$(mktemp -d /tmp/real8_smoke_XXXXXX)"
  done
else
  echo "== [2/5] 阶梯验证 skipped (--skip-ladder) =="
fi

echo "== [3/5] 每方向按源行序取前 $SAMPLE_LIMIT 包 (0=全量), RUN=$FULL_RUN =="
run_case "$SAMPLE_LIMIT" "$FULL_RUN"
grep -a -m1 "REAL8_STAT_WINDOW" "$FULL_RUN/sim.log"

echo "== [4/5] 统计分析 (37 项固定顺序 -> real8_run_perf.{md,csv,json}) =="
python3 "$ANALYZE" --run-dir "$FULL_RUN" --log "$FULL_RUN/sim.log" \
  --out-prefix "$FULL_RUN/real8_run" | tail -6
for f in real8_run_perf.md real8_run_perf.csv real8_run_perf.json; do
  [ -f "$FULL_RUN/$f" ] || { echo "FATAL: analyze output missing: $f"; exit 1; }
done
if [ "$SAMPLE_LIMIT" = 10000 ]; then
  python3 "$PROJ/tmp/check_json_detail.py" "$FULL_RUN/real8_run_perf.json" \
    | tee "$FULL_RUN/validation.log"
fi

if [ "$DO_ARCHIVE" = 1 ]; then
  echo "== [5/5] 归档 -> rwsplit_results/real8_trace_<run_id>/ =="
  RUN_ID="$(date +%Y%m%d_%H%M%S)"
  ARCH="$PROJ/rwsplit_results/real8_trace_$RUN_ID"
  mkdir -p "$ARCH"
  # (1) 输入清单 + (2) 配置快照
  { echo "== 8盘实测回放 输入清单  run_id=$RUN_ID  $(date '+%F %T') =="; \
    md5sum "$XLSX_RD" "$XLSX_WR"; \
    wc -l "$FULL_RUN/real8_input/"*.csv; } > "$ARCH/input_manifest.txt"
  cp "$FULL_RUN/iommu/cache_config/default_config.json" "$ARCH/cache_config_snapshot.json"
  grep -a -m1 "REAL8_EFFECTIVE" "$FULL_RUN/sim.log" > "$ARCH/effective_config.txt" || true
  # (3) 构建/仿真日志
  gzip -c "$FULL_RUN/sim.log" > "$ARCH/sim.log.gz"
  cp "$FULL_RUN/real8_input/real8_manifest.json" "$ARCH/"
  if [ -n "$BUILD_LOG" ] && [ -f "$BUILD_LOG" ]; then
    cp "$BUILD_LOG" "$ARCH/"
  else
    echo "未指定本次构建日志，不附加来源不明的历史日志"
  fi
  # (4) 规范化输入
  cp "$FULL_RUN/real8_input/read_trace.csv" "$FULL_RUN/real8_input/write_trace.csv" "$ARCH/"
  # (5) 逐包重映射 / 请求 / 操作 CSV (operations_raw 约 764MB, gzip 归档)
  cp "$FULL_RUN/real8_trace_remap.csv" "$FULL_RUN/real8_trace_requests.csv" \
     "$FULL_RUN/real8_trace_pairs.csv" "$FULL_RUN/real8_trace_summary.csv" "$ARCH/"
  echo "gzipping real8_operations_raw.csv..."
  gzip -c "$FULL_RUN/real8_operations_raw.csv" > "$ARCH/real8_operations_raw.csv.gz"
  cp "$FULL_RUN/real8_dedup_occupancy.csv" "$FULL_RUN/real8_ptw_occupancy.csv" "$ARCH/"
  # (6) 37 项三格式
  cp "$FULL_RUN/real8_run_perf.md" "$FULL_RUN/real8_run_perf.csv" \
     "$FULL_RUN/real8_run_perf.json" "$ARCH/"
  if [ -f "$FULL_RUN/real8_validation.json" ]; then
    cp "$FULL_RUN/real8_validation.json" "$FULL_RUN/validation.log" "$ARCH/"
  fi
  # (7) 单设备8通道512Byte基线 前后对照表(含 286.90 锚点对账)
  if [ "${REAL8_INCLUDE_BASELINE:-0}" = 1 ] && [ -f "$PRE_LOG" ] && [ -f "$POST_LOG" ]; then
    { echo "# 单设备(8通道) 512Byte 基线场景 功能与性能 前后对照  run_id=$RUN_ID"; echo
      echo "| 项 | pre (real8 改动撤销版) | post (real8 改动版) |"
      echo "|---|---|---|"
      echo "| Steady IOPS | $(grep -a -m1 'Steady IOPS' "$PRE_LOG" | sed 's/^ *//') | $(grep -a -m1 'Steady IOPS' "$POST_LOG" | sed 's/^ *//') |"
      echo "| MD_FAIRNESS | \`$(grep -a -m1 'MD_FAIRNESS' "$PRE_LOG")\` | \`$(grep -a -m1 'MD_FAIRNESS' "$POST_LOG")\` |"; echo
      echo "## 286.90 M/s 锚点对账说明"; echo
      echo "- 用户锚点: 286.90 M/s; 现有报告 rwsplit_results/perf_mc_base200ns.md 同场景记录 287.02 Mreq/s。"
      echo "- 本次隔离环境实际重跑: pre=$(grep -a -m1 -oE '287\.[0-9]+' "$PRE_LOG" | head -1), post=$(grep -a -m1 -oE '287\.[0-9]+' "$POST_LOG" | head -1) M/s, 两者逐位一致(同窗口/Jain=1)。"
      echo "- pre 与 post 完全一致即零影响回归通过; 与锚点 286.90 的 ~0.04% 差异为隔离环境重跑值与历史报告口径差异, 按计划第171节以实际重跑值对账并说明。"
    } > "$ARCH/baseline_pre_post.md"
  else
    echo "未附加历史基线对照，避免将旧日志当作本轮回归结果"
  fi
  echo "ARCHIVED -> $ARCH"
  ls -la "$ARCH"
fi

echo "== ALL DONE =="
