#!/usr/bin/env bash
# Run the three timbre2 tuning variants and compare to the pinned baseline.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_RUN="${BASELINE_RUN:-$SCRIPT_DIR/results/fma_small_20260831T103326Z}"
# Only timbre2_cs is still registered; d05/sh15 were reverted after evaluation
# (eval/TUNING_RESULTS.md). Re-register them to run those arms again.
VARIANTS="${VARIANTS:-timbre2_cs}"
SKIP_DOCKER="${SKIP_DOCKER:-0}"
PYTHON="$SCRIPT_DIR/.venv/bin/python"

if [[ "$SKIP_DOCKER" != "1" ]]; then
  echo "=== Rebuild Docker image (includes tuning methods) ==="
  docker build -t musly:dev "$ROOT"
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TUNING_RUN="$SCRIPT_DIR/results/tuning_${STAMP}"
mkdir -p "$TUNING_RUN"

# timbre2_cs: query-time CSLS only — clone baseline features, skip re-analysis.
if [[ " $VARIANTS " == *" timbre2_cs "* ]]; then
  echo ""
  echo "=== Benchmark: timbre2_cs (clone + dump) ==="
  mkdir -p "$TUNING_RUN/timbre2_cs"
  "$PYTHON" "$SCRIPT_DIR/clone_collection.py" \
    "$BASELINE_RUN/timbre2/collection.musly" \
    "$TUNING_RUN/timbre2_cs/collection.musly" \
    timbre2_cs
  CHECK_DETERMINISM=0 "$SCRIPT_DIR/dump_metrics_only.sh" \
    timbre2_cs "$TUNING_RUN" "$TUNING_RUN/timbre2_cs/collection.musly"

  echo "=== Perturbations: timbre2_cs (clone + dump) ==="
  PERT_OUT="$SCRIPT_DIR/results/perturbations"
  PERT="$SCRIPT_DIR/data/perturbations"
  mkdir -p "$PERT_OUT/timbre2_cs"
  "$PYTHON" "$SCRIPT_DIR/clone_collection.py" \
    "$PERT_OUT/timbre2/collection.musly" \
    "$PERT_OUT/timbre2_cs/collection.musly" \
    timbre2_cs
  rm -f "$PERT_OUT/timbre2_cs/collection.musly.jbox"
  # shellcheck source=lib.sh
  source "$SCRIPT_DIR/lib.sh"
  TREE="$PERT"
  RUN_DIR="$PERT_OUT"
  resolve_musly
  cs_coll="$PERT_OUT/timbre2_cs/collection.musly"
  cs_dump="$PERT_OUT/timbre2_cs/dump.txt"
  musly_run "$PERT_OUT" -- -c "$cs_coll" -J -s "$cs_dump" -k 50
  "$PYTHON" "$SCRIPT_DIR/score_perturbations.py" \
    --dump "$cs_dump" \
    --pairs "$PERT/pairs.csv" \
    --out-json "$PERT_OUT/timbre2_cs/perturbation_metrics.json"
fi

for method in $VARIANTS; do
  [[ "$method" == "timbre2_cs" ]] && continue
  echo ""
  echo "=== Benchmark: $method ==="
  METHODS="$method" OUT_ROOT="$TUNING_RUN" CHECK_DETERMINISM=0 \
    "$SCRIPT_DIR/run_benchmark.sh"

  echo "=== Perturbations: $method ==="
  OUT="$SCRIPT_DIR/results/perturbations" "$SCRIPT_DIR/run_perturbations.sh" "$method"
done

echo ""
echo "=== Summary vs baseline ($BASELINE_RUN) ==="
"$PYTHON" "$SCRIPT_DIR/summarize_tuning.py" \
  --baseline-run "$BASELINE_RUN" \
  --tuning-run "$TUNING_RUN" \
  --out "$TUNING_RUN/summary.json"

echo ""
echo "Tuning results: $TUNING_RUN"
