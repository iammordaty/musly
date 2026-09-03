#!/usr/bin/env bash
# Tuning round 2: covariance shrinkage grid on top of the CSLS hub penalty.
#
# HISTORICAL RECORD. This script describes the arms as they were registered
# during round 2. None of the timbre2_* variants nor the CSLS code path exists
# any more: lambda=0.15 won and is now built into timbre2, and the CSLS
# normalizer was removed. Re-running this requires re-adding those variants.
# Results: eval/results/lambda_20260903T071915Z, write-up in TUNING_RESULTS.md.
#
# Eight arms in a 4x2 design (lambda in {0.10, 0.15, 0.20, 0.25}) x (CSLS off/on):
#
#   lambda  no CSLS          with CSLS
#   0.10    timbre2          timbre2_cs
#   0.15    timbre2_sh15     timbre2_cs_sh15
#   0.20    timbre2_sh20     timbre2_cs_sh20
#   0.25    timbre2_sh25     timbre2_cs_sh25
#
# lambda=0.15 is measured directly rather than interpolated: round 1 showed it
# is the only value known to move hub skewness (2.758 -> 2.491) at a P@10 cost
# that stays inside the neutrality band.
#
# Shrinkage changes the stored features, CSLS is query-time only, so each
# lambda needs exactly one analysis pass and its partner is a header rewrite
# away (eval/clone_collection.py). Four analyses, four clones.
#
# A fresh timbre2 arm is mandatory rather than reusing the pinned baseline:
# the resampler clipping fix changed the features of most tracks, so the old
# run is not comparable (see eval/TUNING_RESULTS.md, experiment A2).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PYTHON="${PYTHON:-$SCRIPT_DIR/.venv/bin/python}"
SKIP_DOCKER="${SKIP_DOCKER:-0}"

# Arms that need a full analysis pass, and the clone derived from each.
FULL_ARMS=(timbre2 timbre2_cs_sh15 timbre2_cs_sh20 timbre2_cs_sh25)
declare -A CLONE_OF=(
  [timbre2_cs]=timbre2
  [timbre2_sh15]=timbre2_cs_sh15
  [timbre2_sh20]=timbre2_cs_sh20
  [timbre2_sh25]=timbre2_cs_sh25
)
CLONE_ARMS=(timbre2_cs timbre2_sh15 timbre2_sh20 timbre2_sh25)
BASE_ARM="timbre2"

if [[ "$SKIP_DOCKER" != "1" ]]; then
  echo "=== Rebuild Docker image ==="
  docker build -t musly:dev "$ROOT"
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_ROOT="$SCRIPT_DIR/results/lambda_${STAMP}"
PERT_OUT="$OUT_ROOT/perturbations"
mkdir -p "$OUT_ROOT" "$PERT_OUT"

echo ""
echo "=== Ranking benchmark: ${FULL_ARMS[*]} (full analysis) ==="
METHODS="${FULL_ARMS[*]}" OUT_ROOT="$OUT_ROOT" CHECK_DETERMINISM=0 \
  "$SCRIPT_DIR/run_benchmark.sh"

RUN="$(cat "$OUT_ROOT/latest.txt")"
echo "Full-analysis arms in $RUN"

for arm in "${CLONE_ARMS[@]}"; do
  src="${CLONE_OF[$arm]}"
  echo ""
  echo "=== Ranking benchmark: $arm (clone of $src, no re-analysis) ==="
  mkdir -p "$RUN/$arm"
  "$PYTHON" "$SCRIPT_DIR/clone_collection.py" \
    "$RUN/$src/collection.musly" \
    "$RUN/$arm/collection.musly" \
    "$arm"
  CHECK_DETERMINISM=0 "$SCRIPT_DIR/dump_metrics_only.sh" \
    "$arm" "$RUN" "$RUN/$arm/collection.musly"
  # Reuse the source arm's analyzed list: same features, same tracks.
  cp "$RUN/$src/analyzed.txt" "$RUN/$arm/analyzed.txt"
done

echo ""
echo "=== Perturbations: ${FULL_ARMS[*]} (full analysis) ==="
for arm in "${FULL_ARMS[@]}"; do
  OUT="$PERT_OUT" "$SCRIPT_DIR/run_perturbations.sh" "$arm"
done

echo ""
echo "=== Perturbations: ${CLONE_ARMS[*]} (clone + dump) ==="
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
PERT="$SCRIPT_DIR/data/perturbations"
TREE="$PERT"
RUN_DIR="$PERT_OUT"
resolve_musly
for arm in "${CLONE_ARMS[@]}"; do
  src="${CLONE_OF[$arm]}"
  mkdir -p "$PERT_OUT/$arm"
  "$PYTHON" "$SCRIPT_DIR/clone_collection.py" \
    "$PERT_OUT/$src/collection.musly" \
    "$PERT_OUT/$arm/collection.musly" \
    "$arm"
  rm -f "$PERT_OUT/$arm/collection.musly.jbox"
  musly_run "$PERT_OUT" -- -c "$PERT_OUT/$arm/collection.musly" \
    -J -s "$PERT_OUT/$arm/dump.txt" -k 50
  "$PYTHON" "$SCRIPT_DIR/score_perturbations.py" \
    --dump "$PERT_OUT/$arm/dump.txt" \
    --pairs "$PERT/pairs.csv" \
    --out-json "$PERT_OUT/$arm/perturbation_metrics.json" > /dev/null
done

echo ""
echo "=== Paired comparison against $BASE_ARM ==="
for arm in "${FULL_ARMS[@]}" "${CLONE_ARMS[@]}"; do
  [[ "$arm" == "$BASE_ARM" ]] && continue
  [[ -f "$RUN/$arm/per_query.csv" ]] || continue
  "$PYTHON" "$SCRIPT_DIR/compare.py" \
    --a-csv "$RUN/$BASE_ARM/per_query.csv" \
    --b-csv "$RUN/$arm/per_query.csv" \
    --a-dump "$RUN/$BASE_ARM/dump.txt" \
    --b-dump "$RUN/$arm/dump.txt" \
    --out-json "$RUN/compare_${BASE_ARM}_vs_${arm}.json" \
    --out-disagreements "$RUN/disagreements_${arm}.csv" > /dev/null
  echo "  wrote compare_${BASE_ARM}_vs_${arm}.json"
done

echo ""
echo "=== Summary ==="
"$PYTHON" "$SCRIPT_DIR/summarize_lambda_grid.py" \
  --run "$RUN" \
  --perturbations "$PERT_OUT" \
  --base "$BASE_ARM" \
  --out "$OUT_ROOT/summary.json"

echo ""
echo "Lambda grid results: $OUT_ROOT"
