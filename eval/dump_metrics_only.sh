#!/usr/bin/env bash
# Dump similarity + compute metrics from an existing collection (no re-analysis).
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

METHOD="${1:?method}"
RUN_DIR="${2:?run_dir}"
COLL="${3:?collection.musly}"
K="${K:-100}"
ARTIST_FIELD="${ARTIST_FIELD:-1}"
FULL_SUBSAMPLE="${FULL_SUBSAMPLE:-2000}"
OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export OMP_NUM_THREADS

TREE="${TREE:-$SCRIPT_DIR/data/tree}"
TREE="$(cd "$TREE" && pwd)"
resolve_musly

mdir="$RUN_DIR/$METHOD"
mkdir -p "$mdir"
dump="$mdir/dump.txt"
full="$mdir/full_subsample.txt"
coll_dest="$mdir/collection.musly"

if [[ "$COLL" != "$coll_dest" ]]; then
  cp "$COLL" "$coll_dest"
fi
rm -f "$coll_dest.jbox"

SUBLIST="$RUN_DIR/subsample.txt"
if [[ ! -f "$SUBLIST" ]]; then
  "$PYTHON" - <<PY
import csv, random, collections
rng = random.Random(42)
rows = list(csv.DictReader(open("$TREE/manifest.csv")))
by = collections.defaultdict(list)
for r in rows:
    by[r["genre"]].append(r["path"])
n = int("$FULL_SUBSAMPLE")
per = max(1, n // max(1, len(by)))
chosen = []
for g, paths in sorted(by.items()):
    rng.shuffle(paths)
    chosen.extend(paths[:per])
rng.shuffle(chosen)
chosen = chosen[:n]
open("$SUBLIST", "w").write("\n".join(chosen) + "\n")
PY
fi

echo "=== dump-only method=$METHOD coll=$coll_dest ==="
musly_run "$RUN_DIR" -- -c "$coll_dest" -J -s "$dump" -k "$K" -f "$ARTIST_FIELD"

subcoll="$mdir/sub.musly"
rm -f "$subcoll" "$subcoll.jbox"
musly_run "$RUN_DIR" -- -n "$METHOD" -c "$subcoll"
# One invocation with repeated -a: a container per track costs minutes.
add_args=()
while IFS= read -r rel; do
  [[ -n "$rel" ]] || continue
  add_args+=(-a "$rel")
done < "$SUBLIST"
musly_run "$TREE" -- -c "$subcoll" "${add_args[@]}"
musly_run "$RUN_DIR" -- -c "$subcoll" -J -m "$full"

"$PYTHON" "$SCRIPT_DIR/metrics.py" \
  --dump "$dump" \
  --full "$full" \
  --out-json "$mdir/metrics.json" \
  --out-csv "$mdir/per_query.csv"

echo "Wrote $mdir/metrics.json"
