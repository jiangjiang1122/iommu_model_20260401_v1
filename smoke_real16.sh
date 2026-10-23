#!/usr/bin/env bash
# [16盘实测回放] 冒烟用例: --limit N 单次运行 + PASS 校验。用法: smoke_real16.sh <N>
set -eu -o pipefail
PROJ="$(cd "$(dirname "$0")" && pwd)"
N="${1:-1000}"
RUN=$(mktemp -d /tmp/real16_smoke_XXXXXX)
mkdir -p "$RUN/real16_input" "$RUN/iommu/cache_config"
cp "$PROJ/iommu/cache_config/default_config.json" "$RUN/iommu/cache_config/"
python3 "$PROJ/rp/prepare_real16_trace.py" --src-dir "$PROJ" --out-dir "$RUN/real16_input" --limit "$N"
( cd "$RUN" && "$PROJ/iommu_model_real16" > sim.log 2>&1 )
grep -a -m1 "REAL16_RESULT" "$RUN/sim.log"
grep -a -m1 "REAL16_STAT_WINDOW" "$RUN/sim.log"
grep -a -m1 "REAL16_TOTAL" "$RUN/sim.log"
grep -a "REAL16_RESIDUAL" "$RUN/sim.log" || true
echo "SMOKE_RUN_DIR=$RUN"
