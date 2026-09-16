#!/usr/bin/env bash
# Build a perturbation collection for one method and score original retrieval.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

PYTHON="${PYTHON:-}"
if [[ -z "$PYTHON" ]]; then
  if [[ -x "$SCRIPT_DIR/.venv/bin/python" ]]; then
    PYTHON="$SCRIPT_DIR/.venv/bin/python"
  else
    PYTHON="python3"
  fi
fi

PERT="${PERT:-$SCRIPT_DIR/data/perturbations}"
OUT="${OUT:-$SCRIPT_DIR/results/perturbations}"
METHOD="${1:-timbre2}"
K="${K:-50}"
OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export OMP_NUM_THREADS

if [[ ! -f "$PERT/pairs.csv" ]]; then
  echo "Missing $PERT/pairs.csv (run make_perturbations.py)" >&2
  exit 1
fi

PERT="$(cd "$PERT" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
# Alias mounts expected by lib.sh (TREE/RUN_DIR always required by musly_run).
TREE="$PERT"
RUN_DIR="$OUT"

resolve_musly

mkdir -p "$OUT/$METHOD"
coll="$OUT/$METHOD/collection.musly"
dump="$OUT/$METHOD/dump.txt"
rm -f "$coll" "$coll.jbox"

echo "Using musly: $MUSLY ($MUSLY_MODE)"
musly_run "$OUT" -- -n "$METHOD" -c "$coll"
musly_run "$PERT" -- -c "$coll" -a originals -x mp3
musly_run "$PERT" -- -c "$coll" -a variants -x mp3

musly_run "$OUT" -- -c "$coll" -J -s "$dump" -k "$K"
"$PYTHON" "$SCRIPT_DIR/score_perturbations.py" \
  --dump "$dump" \
  --pairs "$PERT/pairs.csv" \
  --out-json "$OUT/$METHOD/perturbation_metrics.json"

echo "Wrote $OUT/$METHOD/perturbation_metrics.json"
