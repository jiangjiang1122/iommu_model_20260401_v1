#!/usr/bin/env bash
# =============================================================================
# [N×C 扩展性矩阵 20261106] 6 场景 · fixed_total 恒定总包数 · 全量性能指标对比
#
#   场景1 = 8盘 × 1通道  N=8  C=1  TASKS=250/设备 => 20000 包
#   场景2 = 8盘 × 2通道  N=8  C=2  TASKS=250/设备 => 20000 包
#   场景3 = 8盘 × 4通道  N=8  C=4  TASKS=250/设备 => 20000 包
#   场景4 = 16盘 × 1通道 N=16 C=1  TASKS=124/设备 => 19840 包
#   场景5 = 16盘 × 2通道 N=16 C=2  TASKS=124/设备 => 19840 包
#   场景6 = 16盘 × 4通道 N=16 C=4  TASKS=124/设备 => 19840 包
#
# [为何 16 盘是 19840 而不是 20000] 线程侧 static_assert(TASKS % 2 == 0) 要求每设备
#   任务数为偶数(保证"1读任务+1写任务"严格交替、读写任务数各半)。20000 包 => 2000 任务
#   => N=16 时 125 任务/设备(奇数)不可编译; 取最近的偶数 124 => 19840 包(-0.8%)。
#   N=8 的 3 个场景仍为精确 20000 包。用户 20261106 明确选择"保持偶数约束"。
#
# 用法(WSL 内):
#   bash tmp/nvrr6_matrix_run_all.sh           # 全流程
#   bash tmp/nvrr6_matrix_run_all.sh build     # 只构建 6 个二进制 + 宏集合 diff
#   bash tmp/nvrr6_matrix_run_all.sh run       # 只跑 6 个仿真点(需已构建)
#   bash tmp/nvrr6_matrix_run_all.sh gates     # 只重跑门禁
#   bash tmp/nvrr6_matrix_run_all.sh report    # extract + 6 场景全量指标对比
#   NVRR6_TS=<批次号> bash tmp/nvrr6_matrix_run_all.sh report   # 复用同批次目录
#
# 一致性约定(与 nvrr_pq_run_all.sh 同口径, 便于跨批次比较):
#   1. 全部二进制在同一工作树、同一批次内 make -B 全量重建(Makefile 无依赖跟踪);
#   2. 统一 EXTRA_FLAGS=-DTEST_CFG_DDR_PORT_BEAT_ALIGN=0;
#   3. 运行目录位于 WSL 本地盘 /tmp, 各点独立; 归档只写新目录
#      rwsplit_results/nvrr6_<ts>/, 不覆盖任何历史归档;
#   4. 对象根/二进制统一带 nvrr6_ 前缀, 不覆盖 build/、build_nvrr_n8c*_pq|ft/
#      与 iommu_model_nvrr_* 等既有产物。
# =============================================================================
set -uo pipefail

PROJ=/mnt/d/Qoder_proj/iommu_model_20260401_v1
SK="$PROJ/.qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py"
GATE="$PROJ/tmp/nvrr_gate_check.py"
CMP="$PROJ/rp/compare_nvrr6_matrix.py"
STAGE="${1:-all}"

# 批次号: 显式 NVRR6_TS 优先; 否则 build/all 生成新批次号并落盘, 其余阶段复用最近批次。
TSFILE=/tmp/nvrr6_LATEST_TS
if [ -n "${NVRR6_TS:-}" ]; then
  TS=$NVRR6_TS
elif [ "$STAGE" = "build" ] || [ "$STAGE" = "all" ] || [ ! -f "$TSFILE" ]; then
  TS=$(date +%Y%m%d_%H%M%S); echo "$TS" > "$TSFILE"
else
  TS=$(cat "$TSFILE")
fi
EXTRA="${NVRR6_EXTRA_FLAGS:--DTEST_CFG_DDR_PORT_BEAT_ALIGN=0}"
B0="${NVRR6_B0:-/tmp/nvrr6_$TS}"
ARCH="$PROJ/rwsplit_results/nvrr6_$TS"
NP="$(nproc)"
SIM_TIMEOUT="${NVRR6_SIM_TIMEOUT:-1800}"

# 场景表: tag TEST名 N C TASKS/设备
# [扩展 20261106] 补测场景7(8盘8通道)/场景8(16盘8通道)。
#   场景8 的 TASKS 可由 NVRR6_S8_TASKS 覆盖: 124 => 19840 包(与场景4-6 同规模);
#   128 => 20480 包但队列完全均衡(见下方 pkt_dev_pct 说明)。
NVRR6_S8_TASKS="${NVRR6_S8_TASKS:-124}"
SCENES=(
  "s1_n8c1  rand4k_nvme10_rr_n8c1  8  1 250"
  "s2_n8c2  rand4k_nvme10_rr_n8c2  8  2 250"
  "s3_n8c4  rand4k_nvme10_rr_n8c4  8  4 250"
  "s4_n16c1 rand4k_nvme10_rr_n8c1 16  1 124"
  "s5_n16c2 rand4k_nvme10_rr_n8c2 16  2 124"
  "s6_n16c4 rand4k_nvme10_rr_n8c4 16  4 124"
  "s7_n8c8  rand4k_nvme10_rr_n8c8  8  8 250"
  "s8_n16c8 rand4k_nvme10_rr_n8c8 16  8 ${NVRR6_S8_TASKS}"
)
# 报告用中文场景名(与 SCENES 同序)
LABELS=(
  "场景1 8盘1通道"
  "场景2 8盘2通道"
  "场景3 8盘4通道"
  "场景4 16盘1通道"
  "场景5 16盘2通道"
  "场景6 16盘4通道"
  "场景7 8盘8通道"
  "场景8 16盘8通道"
)
# 场景过滤: NVRR6_ONLY="s7_n8c8 s8_n16c8" 只跑指定 tag(空=全部)。
#   用于补测时不重建/不重跑已归档的场景。
ONLY="${NVRR6_ONLY:-}"
want() {  # want <tag> -> 0=要跑
  [ -z "$ONLY" ] && return 0
  case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

mkdir -p "$B0"
cd "$PROJ" || exit 1
echo "########## NVRR6 MATRIX ts=$TS stage=$STAGE B0=$B0 nproc=$NP $(date '+%F %T')"

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
# 对象根/二进制名: n{N}c{C}_ft 已能唯一区分矩阵里的 8 个点。
#   若后续需要同一 N/C 跑不同 TASKS(如场景8 的 124 vs 128), 用 NVRR6_NAMESUF
#   加一个后缀隔离, 不得静默复用同名产物。
NAMESUF="${NVRR6_NAMESUF:-}"
# [提速 20261109] 对象根可指向 WSL 本地盘(如 NVRR6_BRBASE=/tmp/), 避开 /mnt/d 的 9p 开销。
#   Makefile 用 BUILD_ROOT ?= build + 模式规则 + mkdir -p $(dir $@), 绝对路径可用;
#   TARGET 仍链到工程根(仿真时靠 $PROJ/$BIN 调用), 不受影响。
BRBASE="${NVRR6_BRBASE:-}"
# [额外 make 变量 20261109] 用于 C=8 死锁诊断跑(如 SCENE13_BUFFER=640)。
#   注意: 非空时改变了 IOMMU 侧配置, 产物与场景1-6 不同构, 必须同时用
#   NVRR6_NAMESUF + 独立 NVRR6_TS 隔离, 不得与基线矩阵混入同一张对比表。
EXTRA_MAKE="${NVRR6_EXTRA_MAKE:-}"
br_of() { echo "${BRBASE}build_nvrr6_n$2c$3_ft$NAMESUF"; }      # br_of <tag> <N> <C>
bin_of() { echo "iommu_model_nvrr6_n$2c$3_ft$NAMESUF"; }

build_one() {  # build_one <tag> <test> <N> <C> <tasks>
  local TAG=$1 TEST=$2 ND=$3 CH=$4 TK=$5
  want "$TAG" || { echo "== SKIP BUILD $TAG (不在 NVRR6_ONLY 里)"; return 0; }
  echo "== BUILD $TAG (N=$ND C=$CH TASKS=$TK EXTRA_MAKE='$EXTRA_MAKE') $(date '+%T')"
  # shellcheck disable=SC2086
  make -B -j"$NP" EXTRA_FLAGS="$EXTRA" TEST="$TEST" \
      NVRR_N="$ND" NVRR_LOAD_MODE=fixed_total NVRR_TASKS="$TK" \
      BUILD_ROOT="$(br_of "$TAG" "$ND" "$CH")" \
      TARGET="$(bin_of "$TAG" "$ND" "$CH")" $EXTRA_MAKE > "$B0/build_$TAG.log" 2>&1
  local RC=$?
  echo "   BUILD_RC_$TAG=$RC"
  [ $RC -eq 0 ] || { tail -30 "$B0/build_$TAG.log"; return 1; }
  # EXTRA_FLAGS 必须真的进到编译命令行(防止被 Makefile 变量覆盖后静默丢失)
  local NE
  NE=$(grep -a -c -- "$EXTRA" "$B0/build_$TAG.log")
  echo "   extra_flag_hits=$NE"
  [ "$NE" -gt 0 ] || { echo "GATE_FAIL extra_flags_$TAG"; return 1; }
  grep -a -m1 -- '-c iommu/iommu_top.cc' "$B0/build_$TAG.log" \
    | tr ' ' '\n' | grep -a '^-D' | sort -u > "$B0/macros_$TAG.txt"
  echo "   macros=$(wc -l < "$B0/macros_$TAG.txt") binary=$(bin_of "$TAG" "$ND" "$CH")"
}

do_build() {
  local s
  for s in "${SCENES[@]}"; do
    # shellcheck disable=SC2086
    build_one $s || exit 1
  done
  do_macro_diffs
  do_binary_sha
}

# 宏集合 diff: 证明"6 个二进制只有激励宏(N/C/TASKS)不同, IOMMU 侧配置逐宏一致"
do_macro_diffs() {
  local REF=s1_n8c1 s TAG ND CH TK A B ND_
  [ -f "$B0/macros_$REF.txt" ] || { echo "[skip] 缺 $REF 宏快照"; return 0; }
  echo "== MACRO DIFF (基准 $REF) =="
  for s in "${SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- $s; TAG=$1
    [ "$TAG" = "$REF" ] && continue
    [ -f "$B0/macros_$TAG.txt" ] || { echo "[skip] diff $REF vs $TAG"; continue; }
    diff -u "$B0/macros_$REF.txt" "$B0/macros_$TAG.txt" > "$B0/macro_diff_${REF}_${TAG}.txt"
    ND_=$(grep -a -c '^[-+]-D' "$B0/macro_diff_${REF}_${TAG}.txt" || true)
    echo "-- $REF vs $TAG (${ND_} 行增减, 只允许 NUM_DEVICES / MD_CHAN_PER_DEV / NUM_PAGES / NVRR_TASKS_PER_DEV)"
    grep -a '^[-+]-D' "$B0/macro_diff_${REF}_${TAG}.txt" || echo "   (宏集合完全相同)"
    # 硬门禁: 出现上述 4 个激励宏之外的差异即判失败。
    #   注: macros_*.txt 每行形如 -DTEST_CFG_X=1, 经 diff 后行首为 '-'+'-D' 或 '+'+'-D',
    #   故白名单正则必须以 ^[-+]-D 开头, 否则会把正常激励差异误判为失败。
    local BAD
    BAD=$(grep -a '^[-+]-D' "$B0/macro_diff_${REF}_${TAG}.txt" \
          | grep -av -E '^[-+]-DTEST_CFG_(NUM_DEVICES|MD_CHAN_PER_DEV|NUM_PAGES|NVRR_TASKS_PER_DEV)=' || true)
    if [ -n "$BAD" ]; then
      echo "GATE_FAIL macro_diff_$TAG: 出现激励宏之外的差异:"; echo "$BAD"
    else
      echo "   MACRO_DIFF_OK_$TAG (仅激励宏不同)"
    fi
  done
}

do_binary_sha() {
  local s TAG TEST ND CH TK
  : > "$B0/binary_sha256.txt"
  for s in "${SCENES[@]}"; do
    # SCENES 字段序: $1=tag $2=TEST名 $3=N $4=C $5=TASKS (与 build_one/run_one 一致)
    # shellcheck disable=SC2086
    set -- $s; TAG=$1; TEST=$2; ND=$3; CH=$4; TK=$5
    local BIN
    BIN=$(bin_of "$TAG" "$ND" "$CH")
    if [ -f "$PROJ/$BIN" ]; then
      sha256sum "$BIN" >> "$B0/binary_sha256.txt"
    else
      echo "MISSING  $BIN" >> "$B0/binary_sha256.txt"
    fi
  done
  # 不得静默为空: 空文件意味着上面取字段写错了, 必须显式报错
  if ! grep -aq . "$B0/binary_sha256.txt"; then
    echo "GATE_FAIL binary_sha256 为空(取字段错误?)"; return 1
  fi
  cat "$B0/binary_sha256.txt"
}

# ---------------------------------------------------------------------------
# 运行
# ---------------------------------------------------------------------------
prep_dir() {  # prep_dir <run_dir>
  rm -rf "$1"; mkdir -p "$1/iommu/cache_config"
  # [可选配置 20261113] NVRR6_DEDUP_CONFIG 非空时直接加载该 committed 配置
  #   (如 default_config_dedup512.json), 否则用主配置; 统一落名为 default_config.json。
  local SRC="$PROJ/iommu/cache_config/default_config.json"
  if [ -n "${NVRR6_DEDUP_CONFIG:-}" ]; then
    SRC="$PROJ/iommu/cache_config/$NVRR6_DEDUP_CONFIG"
    [ -f "$SRC" ] || { echo "FATAL: 缺配置 $SRC"; exit 1; }
  fi
  cp "$SRC" "$1/iommu/cache_config/default_config.json"
  # [去重Cache扩容A/B 20261112] NVRR6_DEDUP_SETS 非空时只改"运行目录副本"的
  #   dedup_cache.num_sets; 主配置保持 64, 以保护 real8/real16 线程里
  #   total_lines()==256 的硬断言(改主配置会使实测回放基线不可跑)。
  #   几何为运行时 JSON 读取(cfg.dedup_cache.num_sets), 非编译期宏,
  #   故可复用同一批二进制 => A/B 唯一变量即本参数。
  #   num_ways/num_rams 不动: 128 sets x 4 ways = 512 cachelines, 128%4==0 合法。
  if [ -n "${NVRR6_DEDUP_SETS:-}" ]; then
    python3 - "$1/iommu/cache_config/default_config.json" "$NVRR6_DEDUP_SETS" <<'PY'
import json, sys
p, n = sys.argv[1], int(sys.argv[2])
j = json.load(open(p))
d = j['dedup_cache']
old = d['num_sets']
assert n % d['num_rams'] == 0 and (n & (n - 1)) == 0, 'num_sets 须为2的幂且被 num_rams 整除'
d['num_sets'] = n
json.dump(j, open(p, 'w'), indent=2, ensure_ascii=False)
print('[DEDUP_SETS_OVERRIDE] %d -> %d (ways=%d rams=%d => lines=%d)'
      % (old, n, d['num_ways'], d['num_rams'], n * d['num_ways']))
PY
  fi
}

run_one() {  # run_one <tag> <test> <N> <C> <tasks>
  local TAG=$1 TEST=$2 ND=$3 CH=$4 TK=$5
  local BIN R
  want "$TAG" || { echo "== SKIP RUN $TAG (不在 NVRR6_ONLY 里)"; return 0; }
  BIN=$(bin_of "$TAG" "$ND" "$CH")
  R="$B0/$TAG"
  [ -x "$PROJ/$BIN" ] || { echo "FATAL: 缺二进制 $BIN (先跑 build 阶段)"; exit 1; }
  prep_dir "$R"
  echo "== SIMULATE $TAG (N=$ND C=$CH TASKS=$TK) $(date '+%T')"
  ( cd "$R" && timeout "$SIM_TIMEOUT" "$PROJ/$BIN" > sim.log 2>&1 )
  local RC=$?
  echo "   SIM_RC_${TAG}=$RC"
  [ $RC -eq 0 ] || { echo "FATAL_${TAG}(rc=$RC)"; tail -40 "$R/sim.log"; exit 1; }
  grep -a -m1 'NVRR_RESULT' "$R/sim.log" || { echo "FATAL_${TAG}"; tail -40 "$R/sim.log"; exit 1; }
  grep -a -m1 'MDMC_CONFIG' "$R/sim.log"
  grep -a -m1 'NVRR_PATTERN_OK' "$R/sim.log"
  grep -a -m1 'MDMC_SUPPLY' "$R/sim.log"
  grep -a -m1 'MD_FAIRNESS' "$R/sim.log"
  grep -a 'Steady IOPS' "$R/sim.log" | tail -2
}

do_run() {
  local s
  for s in "${SCENES[@]}"; do
    # shellcheck disable=SC2086
    run_one $s
  done
  echo "== ALL RUNS DONE $(date '+%T')"
}

# ---------------------------------------------------------------------------
# 门禁
# ---------------------------------------------------------------------------
gates_body() {
  local RC=0 s TAG TEST ND CH TK TOT RD WR WARM J PERF
  for s in "${SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- $s; TAG=$1; TEST=$2; ND=$3; CH=$4; TK=$5
    local R="$B0/$TAG"
    # [过滤 20261109] 不在 NVRR6_ONLY 里的场景属"本批次未计划跑", 不得计入失败;
    #   否则补测子集时 GATES 总结会被未跑场景污染成 FAILED。
    if ! want "$TAG"; then echo "[skip] $TAG: 不在本批次范围(NVRR6_ONLY)"; continue; fi
    if [ ! -f "$R/sim.log" ]; then echo "[FAIL] $TAG: 在范围内但缺 $R/sim.log"; RC=1; continue; fi
    # 测量段包数: 每任务 10 包(SQ + 8x512B + CQ), 读写口各 5 包/任务
    TOT=$((ND * TK * 10)); RD=$((ND * TK * 5)); WR=$((ND * TK * 5))
    # 非测量段: 哨兵 2 包/设备 + 预热 8 伪任务 x 2 包/设备 = 18 包/设备
    WARM=$((ND * 18))
    PERF=""; J=$(ls "$R"/perf_scene*.json 2>/dev/null | head -1)
    [ -n "$J" ] && PERF="--perf-json $J"
    echo
    echo "---------- GATE $TAG (N=$ND C=$CH TASKS=$TK total=$TOT rd=$RD wr=$WR warm=$WARM) ----------"
    python3 "$GATE" --tag "$TAG" --log "$R/sim.log" \
      --expect-total "$TOT" --expect-rd "$RD" --expect-wr "$WR" \
      --n "$ND" --c "$CH" --tasks "$TK" --mode fixed_total --warmup "$WARM" \
      $PERF > "$R/gate.txt" 2>&1
    local G=$?
    cat "$R/gate.txt"
    [ $G -eq 0 ] || { echo "GATE_RESULT_${TAG}=FAIL"; RC=1; }
  done
  echo
  if [ $RC -eq 0 ]; then echo "########## GATES ALL PASS ##########"
  else echo "########## GATES FAILED ##########"; fi
  return $RC
}

do_gates() {
  gates_body 2>&1 | tee "$B0/GATES_ALL.txt"
  return "${PIPESTATUS[0]}"
}

# ---------------------------------------------------------------------------
# 提取 + 报告
# ---------------------------------------------------------------------------
do_extract() {
  local s TAG ND CH
  for s in "${SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- $s; TAG=$1; ND=$2; CH=$3
    local R="$B0/$TAG"
    [ -f "$R/sim.log" ] || { echo "[skip] extract $TAG"; continue; }
    python3 "$SK" "$R/sim.log" --md "$R/perf_scene.md" --json "$R/perf_scene.json" > /dev/null \
      || { echo "FATAL extract_$TAG"; exit 1; }
    tail -n 20000 "$R/sim.log" > "$R/sim_tail.txt"
    echo "   extracted perf_scene.{md,json} for $TAG"
  done
}

do_report() {
  local ARGS=() i TAG
  # LABELS 下标严格跟随 SCENES; 缺产物的场景跳过但下标仍前进, 避免标签错位。
  for i in "${!SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- ${SCENES[$i]}; TAG=$1
    [ -f "$B0/$TAG/perf_scene.json" ] || continue
    ARGS+=(--scene "${LABELS[$i]}|$B0/$TAG")
  done
  python3 "$CMP" "${ARGS[@]}" --batch "nvrr6_$TS" --out "$B0/COMPARE6.md" \
    || { echo FATAL_compare6; exit 1; }
  echo "== COMPARE6.md 已生成: $B0/COMPARE6.md"
}

# [稳态口径 20261106] 复用项目既有提取器 rp/extract_nvrr_full_metrics.py:
#   主口径为稳态窗口 W=(t10,t90](完成事务 10%~90%), 峰值/均值均在 W 内重算;
#   同时并列给出"全程"与"注入期"口径以便交叉核对(遵循 ADR: 禁用全程峰值替代稳态)。
do_extended() {
  local ARGS=() i TAG
  for i in "${!SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- ${SCENES[$i]}; TAG=$1
    [ -f "$B0/$TAG/perf_scene.json" ] || continue
    ARGS+=(--scene "${LABELS[$i]}|$B0/$TAG")
  done
  python3 "$PROJ/rp/extract_nvrr_full_metrics.py" "${ARGS[@]}" \
    --batch "nvrr6_$TS" --out "$B0/EXTENDED_METRICS.md" \
    || { echo FATAL_extended; exit 1; }
  echo "== EXTENDED_METRICS.md 已生成(稳态口径): $B0/EXTENDED_METRICS.md"
}

# ---------------------------------------------------------------------------
# 归档
# ---------------------------------------------------------------------------
do_archive() {
  mkdir -p "$ARCH"
  local i s TAG TEST ND CH TK R A
  for i in "${!SCENES[@]}"; do
    # shellcheck disable=SC2086
    set -- ${SCENES[$i]}; TAG=$1; TEST=$2; ND=$3; CH=$4; TK=$5
    R="$B0/$TAG"; A="$ARCH/$TAG"
    [ -d "$R" ] || continue
    mkdir -p "$A"
    [ -f "$R/sim.log" ] && gzip -c "$R/sim.log" > "$A/sim.log.gz"
    cp "$R/sim_tail.txt" "$R/perf_scene.md" "$R/perf_scene.json" "$A/" 2>/dev/null
    cp "$R/gate.txt" "$A/" 2>/dev/null
    cp "$R"/multidev_mc_rwsplit_nvme10_rr_*.csv "$A/" 2>/dev/null
    cp "$R"/ptw_*.csv "$A/" 2>/dev/null
    cp "$R/iommu/cache_config/default_config.json" "$A/cache_config_snapshot.json" 2>/dev/null
    {
      echo "scene_label=${LABELS[$i]}"
      echo "test=$TEST NVRR_N=$ND NVRR_C=$CH NVRR_TASKS=$TK NVRR_LOAD_MODE=fixed_total"
      echo "make=make -B -j$NP EXTRA_FLAGS=$EXTRA TEST=$TEST NVRR_N=$ND NVRR_LOAD_MODE=fixed_total NVRR_TASKS=$TK BUILD_ROOT=$(br_of "$TAG" "$ND" "$CH") TARGET=$(bin_of "$TAG" "$ND" "$CH")"
      grep -a -m1 -E 'MDMC_CONFIG' "$R/sim.log" 2>/dev/null
      grep -a -m1 -E 'NVRR_STIMULUS' "$R/sim.log" 2>/dev/null
      grep -a -m1 -E 'NVRR_RESULT' "$R/sim.log" 2>/dev/null
      grep -a -m1 -E 'MDMC_UNIQUE_OK' "$R/sim.log" 2>/dev/null
      grep -a -m1 -E 'MDMC_SUPPLY' "$R/sim.log" 2>/dev/null
      grep -a -m1 -E 'MDMC_DELTA' "$R/sim.log" 2>/dev/null
      grep -a -m2 -E 'MD_FAIRNESS' "$R/sim.log" 2>/dev/null
    } > "$A/effective_config.txt"
  done
  cp "$B0"/build_*.log "$B0"/macros_*.txt "$B0"/macro_diff_*.txt "$B0"/binary_sha256.txt "$ARCH/" 2>/dev/null
  cp "$B0/COMPARE6.md" "$B0/EXTENDED_METRICS.md" "$B0/GATES_ALL.txt" "$ARCH/" 2>/dev/null
  cp "$CMP" "$GATE" "$PROJ/tmp/nvrr6_matrix_run_all.sh" "$ARCH/" 2>/dev/null
  {
    echo "run_id=$TS  generated=$(date '+%F %T')"
    echo "extra_make=${EXTRA_MAKE:-<none>}  namesuf=${NAMESUF:-<none>}"
    echo "only=${ONLY:-<all 8 scenes>}"
    echo "batch=same_worktree_same_session(make -B 全量重建, EXTRA_FLAGS=$EXTRA)"
    echo "matrix=完整矩阵为 N×C={8,16}×{1,2,4,8} 共8点, 全部 fixed_total(每设备任务数与C无关); 本批次实跑范围见上行 only="
    echo "stimulus=nvme10(SQ64B,DATA8x512B,CQ16B;无MSI) 严格读写交替 同索引入队 逐包嵌套RR仲裁 每端口1个发送进程"
    echo "thread=rp/test_rp_nvme10_rr_arb_thread.cc (TEST=rand4k_nvme10_rr_n8c{1,2,4} + NVRR_N 覆盖到16)"
    echo "packets: N=8 -> 250任务/设备 x10包 x8 = 20000(精确); N=16 C<=4 -> 124任务/设备 = 19840(-0.8%); N=16 C=8 -> 128任务/设备 = 20480(+2.4%, 为使 64 对任务恰好均分到 8 队列)"
    echo "why_19840=线程 static_assert(TASKS%2==0) 要求每设备任务数为偶数以保证读写任务各半; 20000包需125(奇数)任务/设备, 故取最近偶数124"
    echo "c8_deadlock=C=8 两点在 pt_update_fifo=32 时**环形等待死锁**: 推进停在 t=21005ns(N=8)/29160ns(N=16), 现场 buffer=320(满) ptw=0 groups=0 output=0, 在途=sent-responded=640=Global额度上限; 已调度进 agg_flush_peq 174 组仅走完 61 组, 积压 113 组(=watchdog aggregate=113); 主占位 147创建/38释放 => 112 条链头成为永久孤儿; watchdog 50us 无推进后 abort(rc=134)"
    echo "c8_rootcause_VERIFIED=环形等待(与 iommu_perf_params.hh 旧注释记载同构): agg_flush 阻塞于 pt_update_fifo.write(32满) -> pt_hash_thread 阻塞于 pt_ram_fifo.write -> pt_ram_worker 阻塞于 dedup_request_fifo.write(PT MISS 转发, 64满) -> dedup_scheduler 被 Buffer 满的准入控制抑制不出队 -> Buffer 释放只能靠 dedup_update, 而其唯一生产者 agg_flush 正阻塞在环首"
    echo "c8_why_release_first_insufficient=TEST_CFG_AGG_FLUSH_RELEASE_FIRST=1 只保证**本组**释放先于**本组**回填; agg_flush 是单线程一次一个组, 卡在第N组回填就回不到循环头取第N+1组 => 后续组释放消息永远生产不出来(死锁从组内转为组间)"
    echo "c8_falsified_hypotheses=已排除两条错误假设: (1) 'pt_update_fifo 反压不是成因' —— 该结论依据 [MONITOR_PT_UPD] 计数=0, 但该计数器在阻塞写**返回后**才自增, 无法观测永久阻塞, 结论不成立; (2) '去重侧共享深度1出口队头阻塞' —— 已实现 update 优先通道并实测 UPD 通道反压 0 次(通道确实畅通), 死锁照旧 => 堵点不在去重侧"
    echo "c8_fix_ADOPTED=PT_UPDATE_FIFO_DEPTH 默认值 32 -> 1024(iommu_perf_params.hh)。依据: 回填消息数上界为在途组数x每组iova数, 在途任务受 Global 640 约束; 与同文件 FIFO_DEPTH_PT_CACHE_TO_FWD=1024('[防死锁] 需容纳全部在途任务')同源。实测 pt_update_fifo Peak 仅 44~185 / 1024 (4.3%~18.1%), 余量不可耗尽 => agg_flush 永不阻塞 => 环不闭合"
    echo "c8_fix_verdict=治标而非治本: 靠'余量不可耗尽'避免闭环, 环形依赖本身仍在。治本需解开环上唯一充分边: pt_ram_worker 向 dedup_request_fifo 的阻塞写(PT MISS 转发), 或让 agg_flush 回填写入改为非阻塞+spill"
    echo "c8_ab_evidence=修复前同码 A/B(8盘8通道,20000包): Buffer=640/D=3 -> PASS 稳态ALL=261.38 Admission stall=0; Buffer=320/D=1 -> PASS 稳态ALL=263.70 Admission stall=653次/5414ns; pt_update_fifo=1024 -> PASS 稳态ALL=255.96 Admission stall=555次/4817ns(三条都能解, 最终采用后者因它不动性能相关容量)"
    echo "sideeffect=PT_UPDATE_FIFO_DEPTH 参与时序, 改为1024后**所有**场景绝对吞吐漂移; 本批次场景1-6 与 pt_update_fifo=32 旧批次对比漂移 -3.32%~+1.43%(最大在场景6); 历史基线(286.965/249.93 Mreq/s)不再逐位可复现, 如需复现显式传 -DTEST_CFG_PT_UPDATE_FIFO_DEPTH=32"
    echo "dead_code_note=TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE(去重 update 优先通道) 已证对本死锁无效, 默认 0 关闭, 代码保留作为纵深防御; lane=0 时行为与改动前逐位一致(仅 [CACHE_SUBSYSTEM] 日志多一个 upd_prio_lane=0 字段)"
    echo "queue_depth=0/auto(TASKS+8, 无界) => 构造性饱和, 供给门禁为硬门禁"
    echo "write_order=global_single_FIFO(PERDEV_WRITE_ORDER=0)"
    echo "iommu_config=Global/Read/Write=640/320/320 PTW=64 Buffer=320 D=3 ADM=1 PORT_WIDTH_BIT=1024 SLINK=50ns DDR=100ns Walker+S2Walker=on SPLIT_RW=1 IOVA=512MB PT_UPDATE_FIFO=1024(本批次新默认值)"
    echo "metric_tool=.qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py (8点同一脚本)"
    echo "compare_tool=rp/compare_nvrr6_matrix.py (行=指标 列=场景 的全量并列对比)"
    echo "steady_tool=rp/extract_nvrr_full_metrics.py (项目既有稳态口径提取器 -> EXTENDED_METRICS.md)"
    echo "PRIMARY_CALIBER=稳态窗口 W=(t10,t90] 完成事务的 10%~90%; 峰值/均值均在 W 内重算(ADR: 禁用全程峰值或注入期均值替代)"
    echo "iops_two_calibers=稳态 IOPS = [MD_FAIRNESS] ALL Mreq/s; 全程 IOPS = iommu_iops(含建表/预热/收尾排空); 两者不得混用"
    echo "gate_tool=tmp/nvrr_gate_check.py (本批次为其做了三处修正, 均只改只读校验脚本, 不动模型)"
    echo "gate_fix1=NVRR_ORDER 期望长度改为 min(32,4N): 线程侧 order_sample 恒只存 32 包, N=16 时 4 轮设备RR 需 64 包, 原写法会误判包序不符"
    echo "gate_fix2=head_text 窗口 4MB->16MB: N=16 建表段输出达 4.26MB, 会把 [MDMC_UNIQUE_OK]/[NVRR_PATTERN_OK] 挤出窗口而误报缺行(N=8 在 2.13MB 内未暴露)"
    echo "gate_fix3=端口三分法门禁由 abs(idle)<=0.5% 改为 idle<=0.5%(只卡正向空闲): idle=(span-beat-blk)/span 为负意味端口零空闲(记账窗口小幅超出), 与供给不足方向相反; 供给充足性由 starve_cnt/starve_ns=0 与 empty_sel=0/gen_block_ns=0/prefill_ok=1 独立证明"
    echo "caveat_pt_data_hit=PT 512B 数据命中率在场景1/4 为小负值: 模型侧 ctrl_pt_hits(3998) > pt_cache.hits(3840), 两个计数器作用域不完全重合(差158包≈0.8%), skill 按定义相减得负; 非本批次引入"
    echo "steady_window=10%~90%"
  } > "$ARCH/CALIBER.txt"
  echo "ARCHIVED -> $ARCH"
  ls -la "$ARCH"
}

case "$STAGE" in
  build)    do_build; echo "== BUILDS DONE $(date '+%T')" ;;
  diffs)    do_macro_diffs; do_binary_sha ;;
  run)      do_run ;;
  gates)    do_gates ;;
  extract)  do_extract ;;
  report)   do_extract; do_report; do_extended ;;
  compare)  do_report ;;
  extended) do_extended ;;
  archive)  do_archive ;;
  all)      do_build; do_run; do_extract; do_gates; do_report; do_extended; do_archive ;;
  *) echo "unknown stage: $STAGE (build|diffs|run|gates|extract|report|compare|extended|archive|all)"; exit 2 ;;
esac
echo "########## STAGE=$STAGE DONE $(date '+%F %T')"
