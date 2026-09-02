#!/usr/bin/env bash
# Score the gain/clipping fixture set (eval/make_gain_fixtures.py) per method.
# Separate from run_perturbations.sh because the variants mix mp3 and wav.
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

PERT="${PERT:-$SCRIPT_DIR/data/gaincheck}"
OUT="${OUT:-$SCRIPT_DIR/results/gaincheck}"
METHODS="${METHODS:-timbre timbre2 timbre2_cs}"
K="${K:-50}"
OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export OMP_NUM_THREADS

if [[ ! -f "$PERT/pairs.csv" ]]; then
  echo "Missing $PERT/pairs.csv (run make_gain_fixtures.py)" >&2
  exit 1
fi

PERT="$(cd "$PERT" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
# Alias mounts expected by lib.sh.
TREE="$PERT"
RUN_DIR="$OUT"

resolve_musly
echo "Using musly: $MUSLY ($MUSLY_MODE)"

for method in $METHODS; do
  echo ""
  echo "=== gaincheck: $method ==="
  mkdir -p "$OUT/$method"
  coll="$OUT/$method/collection.musly"
  dump="$OUT/$method/dump.txt"
  rm -f "$coll" "$coll.jbox"

  musly_run "$OUT" -- -n "$method" -c "$coll"
  musly_run "$PERT" -- -c "$coll" -a originals -x mp3
  musly_run "$PERT" -- -c "$coll" -a variants/clip_p6 -x mp3
  musly_run "$PERT" -- -c "$coll" -a variants/f32_p6 -a variants/f32_p0 -x wav

  musly_run "$OUT" -- -c "$coll" -J -s "$dump" -k "$K"
  "$PYTHON" "$SCRIPT_DIR/score_perturbations.py" \
    --dump "$dump" \
    --pairs "$PERT/pairs.csv" \
    --out-json "$OUT/$method/gain_metrics.json" > /dev/null
  "$PYTHON" - <<PY
import json
d = json.load(open("$OUT/$method/gain_metrics.json"))
for tag, v in sorted(d["by_variant"].items()):
    print(f"  {tag:<10} n={v['n']:>3}  top1={v['top1']:.3f}  mrr={v['mrr']:.3f}  median_rank={v['median_rank']:g}")
PY
done

echo ""
echo "Results in $OUT"
