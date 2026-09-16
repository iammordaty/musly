#!/usr/bin/env bash
# Build collections for timbre and timbre2 on the same prepared tree, dump
# sparse (and optional full) similarity matrices, and compute metrics.
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

TREE="${TREE:-$SCRIPT_DIR/data/tree}"
OUT_ROOT="${OUT_ROOT:-$SCRIPT_DIR/results}"
METHODS="${METHODS:-timbre timbre2}"
K="${K:-100}"
GENRE_FIELD="${GENRE_FIELD:-0}"
ARTIST_FIELD="${ARTIST_FIELD:-1}"
FULL_SUBSAMPLE="${FULL_SUBSAMPLE:-2000}"
CHECK_DETERMINISM="${CHECK_DETERMINISM:-0}"
OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
export OMP_NUM_THREADS

TREE="$(cd "$TREE" && pwd)"
resolve_musly

if [[ ! -f "$TREE/manifest.csv" ]]; then
  echo "Missing $TREE/manifest.csv — run prepare_dataset.py first" >&2
  exit 1
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="$OUT_ROOT/fma_small_${STAMP}"
mkdir -p "$RUN_DIR"
RUN_DIR="$(cd "$RUN_DIR" && pwd)"
OUT_ROOT="$(cd "$OUT_ROOT" && pwd)"

echo "Using musly: $MUSLY ($MUSLY_MODE)"
echo "Tree: $TREE"
echo "Results: $RUN_DIR"

manifest_sha="$("$PYTHON" -c "import json;print(json.load(open('$TREE/manifest.json'))['manifest_sha256'])")"
musly_info="$(musly_run "$RUN_DIR" -- -i 2>&1 || true)"
ffmpeg_ver="$(ffmpeg -version 2>/dev/null | head -n1 || echo unknown)"

# Stratified subsample list for full-matrix AUC (optional).
SUBLIST="$RUN_DIR/subsample.txt"
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
print(f"subsample {len(chosen)} tracks -> $SUBLIST")
PY

run_method() {
  local method="$1"
  local mdir="$RUN_DIR/${method}"
  mkdir -p "$mdir"
  local coll="$mdir/collection.musly"
  local dump="$mdir/dump.txt"
  local full="$mdir/full_subsample.txt"

  echo "=== method=$method ==="
  rm -f "$coll" "$coll.jbox"
  musly_run "$RUN_DIR" -- -n "$method" -c "$coll"

  # Add each genre directory so stored paths are genre/artist/track.mp3
  (
    cd "$TREE"
    find . -mindepth 1 -maxdepth 1 -type d ! -name '.*' | sort | while IFS= read -r g; do
      g="${g#./}"
      musly_run "$TREE" -- -c "$coll" -a "$g" -x mp3
    done
  )

  # List analyzed paths for intersection bookkeeping
  musly_run "$RUN_DIR" -- -c "$coll" -l \
    | sed -n 's/.*track-origin: //p' | sort > "$mdir/analyzed.txt"

  musly_run "$RUN_DIR" -- -c "$coll" -J -s "$dump" -k "$K" -f "$ARTIST_FIELD"

  if [[ "$CHECK_DETERMINISM" == "1" ]]; then
    local dump2="$mdir/dump_repeat.txt"
    musly_run "$RUN_DIR" -- -c "$coll" -J -s "$dump2" -k "$K" -f "$ARTIST_FIELD"
    local h1 h2
    h1="$(sha256_file "$dump")"
    h2="$(sha256_file "$dump2")"
    if [[ "$h1" != "$h2" ]]; then
      echo "Determinism check FAILED for $method" >&2
      exit 1
    fi
    echo "Determinism OK ($h1)"
  fi

  # Full matrix on a small temporary collection for AUC
  local subcoll="$mdir/sub.musly"
  rm -f "$subcoll" "$subcoll.jbox"
  musly_run "$RUN_DIR" -- -n "$method" -c "$subcoll"
  # One invocation with repeated -a: a container per track costs minutes.
  local -a add_args=()
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

  "$PYTHON" - <<PY
import json, hashlib
info = {
  "method": "$method",
  "musly": """$musly_info""",
  "musly_bin": "$MUSLY",
  "musly_mode": "$MUSLY_MODE",
  "manifest_sha256": "$manifest_sha",
  "k": int("$K"),
  "artist_field": int("$ARTIST_FIELD"),
  "genre_field": int("$GENRE_FIELD"),
  "omp_num_threads": int("$OMP_NUM_THREADS"),
  "ffmpeg": "$ffmpeg_ver",
  "dump_sha256": hashlib.sha256(open("$dump","rb").read()).hexdigest(),
  "n_analyzed": sum(1 for _ in open("$mdir/analyzed.txt")),
}
json.dump(info, open("$mdir/run.json","w"), indent=2)
print(open("$mdir/run.json").read())
PY
}

for m in $METHODS; do
  run_method "$m"
done

# Intersect analyzed paths across methods
"$PYTHON" - <<PY
import pathlib
root = pathlib.Path("$RUN_DIR")
sets = {}
for p in root.iterdir():
    f = p / "analyzed.txt"
    if f.is_file():
        sets[p.name] = set(x.strip() for x in f.read_text().splitlines() if x.strip())
if len(sets) >= 2:
    keys = sorted(sets)
    inter = set.intersection(*(sets[k] for k in keys))
    print(f"analyzed intersection: {len(inter)}")
    for k in keys:
        only = sets[k] - inter
        print(f"  only in {k}: {len(only)}")
    (root / "analyzed_intersection.txt").write_text("\n".join(sorted(inter)) + "\n")
PY

if [[ -f "$RUN_DIR/timbre/per_query.csv" && -f "$RUN_DIR/timbre2/per_query.csv" ]]; then
  "$PYTHON" "$SCRIPT_DIR/compare.py" \
    --a-csv "$RUN_DIR/timbre/per_query.csv" \
    --b-csv "$RUN_DIR/timbre2/per_query.csv" \
    --a-dump "$RUN_DIR/timbre/dump.txt" \
    --b-dump "$RUN_DIR/timbre2/dump.txt" \
    --out-json "$RUN_DIR/compare.json" \
    --out-disagreements "$RUN_DIR/disagreements.csv"
fi

echo "Done. Results in $RUN_DIR"
echo "$RUN_DIR" > "$OUT_ROOT/latest.txt"
