#!/usr/bin/env bash
# =============================================================================
# [16盘实测回放] 一键串联: 预处理(组B虚拟生成) → 构建 → 40000包仿真 → 37项统计
#                → 16盘 vs 8盘对比 → 归档
#
# 用法(WSL 内):
#   bash run_real16_trace.sh                     # 默认每文件前10000包(共40000), 不跑阶梯
#   bash run_real16_trace.sh --fresh-build       # 使用 make -B 全量重编
#   bash run_real16_trace.sh --skip-build        # 复用二进制, 运行+统计+对比
#   bash run_real16_trace.sh --ladder            # 先跑 --limit 1000 阶梯冒烟
#   bash run_real16_trace.sh --archive           # 完成后归档到 rwsplit_results/real16_trace_<run_id>/
#
# 注意:
#   1. 本场景使用独立对象根 build_real16/ 与独立二进制 iommu_model_real16,
#      绝不清理工程根 build/ 或 build_real8/, 不覆盖既有 iommu_model* 与历史结果。
#   2. Makefile 无 -MMD 依赖跟踪: 修改过 iommu_top.hh 等头文件后必须 --fresh-build
#      全量重编, 否则旧 .o 与新头文件类布局不一致会在运行期产生段错误。
#   3. 运行目录全部位于 WSL 本地盘 /tmp/real16_*, 避免 drvfs 逐行写入开销。
#   4. 组B数据 = 组A内存虚拟副本(request id 逐行+8), 不需要 _1.xlsx 文件。
# =============================================================================
set -eu -o pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
XLSX_RD="$PROJ/4k_randrw_mem_1021_MRd.xlsx"
XLSX_WR="$PROJ/4k_randrw_mem_1021_MWr.xlsx"
PREP="$PROJ/rp/prepare_real16_trace.py"
ANALYZE="$PROJ/rp/analyze_real16_trace.py"
COMPARE="$PROJ/rp/compare_real16_vs_real8.py"
R8_JSON="$PROJ/rwsplit_results/real8_trace_20261022_100602/real8_run_perf.json"
BIN="$PROJ/iommu_model_real16"
FULL_RUN="$(mktemp -d /tmp/real16_run_XXXXXX)"
LOG_BASE=/tmp/real16_logs

FRESH_BUILD=0; SKIP_BUILD=0; SKIP_LADDER=1; DO_ARCHIVE=0; SAMPLE_LIMIT=10000
BUILD_LOG=""
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

echo "== [0/6] 环境与输入检查 =="
command -v python3 >/dev/null || { echo "FATAL: python3 not found"; exit 1; }
[ -f "$XLSX_RD" ] || { echo "FATAL: missing $XLSX_RD"; exit 1; }
[ -f "$XLSX_WR" ] || { echo "FATAL: missing $XLSX_WR"; exit 1; }
echo "inputs ok: MRd.xlsx / MWr.xlsx (组B由 prepare 脚本内存虚拟生成)"

echo "== [1/6] 构建 (TEST=real16_trace_replay -> build_real16/, iommu_model_real16) =="
if [ "$SKIP_BUILD" = 1 ] && [ -x "$BIN" ]; then
  echo "skip build (--skip-build, reuse $BIN)"
else
  BUILD_FLAGS=()
  if [ "$FRESH_BUILD" = 1 ]; then
    echo "--fresh-build: 强制重编全部对象，不删除已有目录"
    BUILD_FLAGS=(-B)
  fi
  BUILD_LOG="$LOG_BASE/build_real16_$(date +%Y%m%d_%H%M%S).log"
  make -C "$PROJ" "${BUILD_FLAGS[@]}" -j"$(nproc)" TEST=real16_trace_replay \
    2>&1 | tee "$BUILD_LOG"
fi
[ -x "$BIN" ] || { echo "FATAL: $BIN not built"; exit 1; }

# 运行单个用例: run_case <N|0=全量> <run_dir>
run_case() {
  local N="$1" RUN="$2"
  local LIMIT=(--limit "$N")
  mkdir -p "$RUN/real16_input" "$RUN/iommu/cache_config"
  cp "$PROJ/iommu/cache_config/default_config.json" "$RUN/iommu/cache_config/"
  echo "-- prepare ($RUN)"
  python3 "$PREP" --src-dir "$PROJ" --out-dir "$RUN/real16_input" ${LIMIT[@]+"${LIMIT[@]}"}
  echo "-- simulate ($RUN)"
  ( cd "$RUN" && REAL16_LIMIT="$N" "$BIN" > sim.log 2>&1 )
  grep -aq "REAL16_RESULT] PASS" "$RUN/sim.log" || {
    echo "FATAL: $RUN simulation not PASS"; tail -30 "$RUN/sim.log"; exit 1; }
  grep -a -m1 "REAL16_RESULT" "$RUN/sim.log"
}

if [ "$SKIP_LADDER" = 0 ]; then
  echo "== [2/6] 阶梯冒烟 每文件前1000包(共8000包) =="
  run_case 1000 "$(mktemp -d /tmp/real16_smoke_XXXXXX)"
else
  echo "== [2/6] 阶梯冒烟 skipped (--skip-ladder) =="
fi

echo "== [3/6] 正式: 每文件前 $SAMPLE_LIMIT 包(0=全量), RUN=$FULL_RUN =="
run_case "$SAMPLE_LIMIT" "$FULL_RUN"
grep -a -m1 "REAL16_STAT_WINDOW" "$FULL_RUN/sim.log"
grep -a -m1 "REAL16_MERGE" "$FULL_RUN/sim.log" || true

echo "== [4/6] 统计分析 (37 项固定顺序 -> real16_run_perf.{md,csv,json}) =="
python3 "$ANALYZE" --run-dir "$FULL_RUN" --log "$FULL_RUN/sim.log" \
  --out-prefix "$FULL_RUN/real16_run" 2>&1 | tee "$FULL_RUN/analyze.log" | tail -8
for f in real16_run_perf.md real16_run_perf.csv real16_run_perf.json; do
  [ -f "$FULL_RUN/$f" ] || { echo "FATAL: analyze output missing: $f"; exit 1; }
done
# 16盘专项硬门禁: 两组独立映射 + 轮询交替 + 逐设备对账 + 池用量
grep -aq "mapped_page_disjoint=True" "$FULL_RUN/analyze.log" || {
  echo "FATAL: 两组独立映射断言未通过"; exit 1; }
grep -aq "interleave_ok=True" "$FULL_RUN/analyze.log" || {
  echo "FATAL: 逐包交替轮询断言未通过"; exit 1; }
grep -aq "per_dev_match=True" "$FULL_RUN/analyze.log" || {
  echo "FATAL: 16设备逐设备对账未通过"; exit 1; }
echo "16盘专项验收(独立映射/轮询/对账): PASS"

echo "== [5/6] 16盘 vs 8盘 37项对比 =="
if [ -f "$R8_JSON" ]; then
  python3 "$COMPARE" --r16-json "$FULL_RUN/real16_run_perf.json" \
    --r8-json "$R8_JSON" --out-prefix "$FULL_RUN/real16_vs_real8"
  grep -a -m1 "IOPS 16/8" "$FULL_RUN/real16_vs_real8_compare.md" || true
  grep -a "IOPS 16/8" "$FULL_RUN/analyze.log" >/dev/null 2>&1 || true
else
  echo "WARN: 8盘基线 JSON 不存在($R8_JSON), 跳过对比"
fi

if [ "$DO_ARCHIVE" = 1 ]; then
  echo "== [6/6] 归档 -> rwsplit_results/real16_trace_<run_id>/ =="
  RUN_ID="$(date +%Y%m%d_%H%M%S)"
  ARCH="$PROJ/rwsplit_results/real16_trace_$RUN_ID"
  mkdir -p "$ARCH"
  # (1) 输入清单 + (2) 配置快照
  { echo "== 16盘实测回放 输入清单  run_id=$RUN_ID  $(date '+%F %T') =="; \
    echo "组B=组A内存虚拟副本(request id逐行+8), 源文件如下:"; \
    md5sum "$XLSX_RD" "$XLSX_WR"; \
    wc -l "$FULL_RUN/real16_input/"*.csv; } > "$ARCH/input_manifest.txt"
  cp "$FULL_RUN/iommu/cache_config/default_config.json" "$ARCH/cache_config_snapshot.json"
  grep -a -m1 "REAL16_EFFECTIVE" "$FULL_RUN/sim.log" > "$ARCH/effective_config.txt" || true
  # (3) 构建/仿真日志
  gzip -c "$FULL_RUN/sim.log" > "$ARCH/sim.log.gz"
  cp "$FULL_RUN/real16_input/real16_manifest.json" "$ARCH/"
  if [ -n "$BUILD_LOG" ] && [ -f "$BUILD_LOG" ]; then
    cp "$BUILD_LOG" "$ARCH/"
  else
    echo "未指定本次构建日志，不附加来源不明的历史日志"
  fi
  # (4) 规范化输入(4份)
  cp "$FULL_RUN/real16_input/read_trace.csv" "$FULL_RUN/real16_input/write_trace.csv" \
     "$FULL_RUN/real16_input/read_trace_1.csv" "$FULL_RUN/real16_input/write_trace_1.csv" "$ARCH/"
  # (5) 逐包重映射 / 请求 / 配对 / 操作 CSV
  cp "$FULL_RUN/real16_trace_remap.csv" "$FULL_RUN/real16_trace_requests.csv" \
     "$FULL_RUN/real16_trace_pairs.csv" "$FULL_RUN/real16_trace_summary.csv" "$ARCH/" 2>/dev/null || true
  echo "gzipping real16_operations_raw.csv..."
  gzip -c "$FULL_RUN/real16_operations_raw.csv" > "$ARCH/real16_operations_raw.csv.gz"
  cp "$FULL_RUN/real16_dedup_occupancy.csv" "$FULL_RUN/real16_ptw_occupancy.csv" "$ARCH/"
  # (6) 37 项三格式 + 对比表 + 分析日志
  cp "$FULL_RUN/real16_run_perf.md" "$FULL_RUN/real16_run_perf.csv" \
     "$FULL_RUN/real16_run_perf.json" "$ARCH/"
  cp "$FULL_RUN/analyze.log" "$ARCH/"
  [ -f "$FULL_RUN/real16_vs_real8_compare.md" ] && \
    cp "$FULL_RUN/real16_vs_real8_compare.md" "$FULL_RUN/real16_vs_real8_compare.csv" "$ARCH/"
  echo "ARCHIVED -> $ARCH"
  ls -la "$ARCH"
fi

echo "== ALL DONE =="
