#!/usr/bin/env bash
# Run registered timbre2 tuning variants and compare them to a baseline run.
#
# No tuning variant is registered in the library right now: rounds 1 and 2 are
# closed and the winning configuration lives in timbre2 itself. Pass the names
# of freshly registered variants in VARIANTS to use this runner again, e.g.
#   VARIANTS="timbre2_foo" eval/run_tuning.sh
# A variant that only changes query-time behaviour can skip re-analysis by
# cloning an existing collection with eval/clone_collection.py.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_RUN="${BASELINE_RUN:-$SCRIPT_DIR/results/lambda_20260903T071915Z/fma_small_20260903T071915Z}"
VARIANTS="${VARIANTS:-}"
SKIP_DOCKER="${SKIP_DOCKER:-0}"
PYTHON="$SCRIPT_DIR/.venv/bin/python"

if [[ -z "${VARIANTS// }" ]]; then
  echo "No variants requested: set VARIANTS to registered method names." >&2
  exit 1
fi

if [[ "$SKIP_DOCKER" != "1" ]]; then
  echo "=== Rebuild Docker image (includes tuning methods) ==="
  docker build -t musly:dev "$ROOT"
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TUNING_RUN="$SCRIPT_DIR/results/tuning_${STAMP}"
mkdir -p "$TUNING_RUN"

for method in $VARIANTS; do
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
