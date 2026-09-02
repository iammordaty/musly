#!/usr/bin/env python3
"""Build a perturbation robustness set from the prepared FMA tree.

For each selected original track, create variants with ffmpeg. The evaluation
protocol queries with a variant and expects the original as the top neighbor.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import shutil
import subprocess
import sys
from typing import List, Optional, Tuple


# (tag, args_before_input, args_after_input)
VARIANTS: List[Tuple[str, List[str], List[str]]] = [
    ("vol_m12", [], ["-af", "volume=-12dB"]),
    ("vol_p6", [], ["-af", "volume=6dB"]),
    ("mp3_64k", [], ["-b:a", "64k"]),
    ("lowpass8k", [], ["-af", "lowpass=f=8000"]),
    ("rate16k", [], ["-ar", "16000"]),
    ("silence_pre20", [], ["-af", "adelay=20000|20000"]),
    ("mid15s", [], ["-ss", "7.5", "-t", "15"]),
    # Loop past 4 minutes so multi-segment analysis is exercised.
    ("loop4m", ["-stream_loop", "9"], ["-t", "260"]),
]


def run_ffmpeg(src: str, dst: str, before: List[str], after: List[str]) -> bool:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    cmd = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        *before,
        "-i",
        src,
        *after,
        "-c:a",
        "libmp3lame",
        dst,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=180)
        return r.returncode == 0 and os.path.isfile(dst) and os.path.getsize(dst) > 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--tree",
        default=os.path.join(os.path.dirname(__file__), "data", "tree"),
        help="Prepared genre/artist tree",
    )
    ap.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "data", "perturbations"),
        help="Output directory for originals + variants",
    )
    ap.add_argument("--n", type=int, default=200, help="Number of source tracks")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    manifest = os.path.join(args.tree, "manifest.csv")
    if not os.path.isfile(manifest):
        raise SystemExit(f"Missing {manifest}; run prepare_dataset.py first")

    rows = list(csv.DictReader(open(manifest)))
    rng = random.Random(args.seed)
    rng.shuffle(rows)
    selected = rows[: args.n]

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)

    pairs = []
    failed = []
    for row in selected:
        rel = row["path"]
        src = os.path.join(args.tree, rel)
        orig_rel = os.path.join("originals", rel)
        orig_dst = os.path.join(args.out, orig_rel)
        os.makedirs(os.path.dirname(orig_dst), exist_ok=True)
        real = os.path.realpath(src)
        if not os.path.isfile(real):
            failed.append({"path": rel, "reason": "missing_original"})
            continue
        shutil.copy2(real, orig_dst)

        for tag, before, after in VARIANTS:
            var_rel = os.path.join("variants", tag, rel)
            var_dst = os.path.join(args.out, var_rel)
            ok = run_ffmpeg(orig_dst, var_dst, before, after)
            if not ok:
                failed.append({"path": rel, "variant": tag, "reason": "ffmpeg"})
                continue
            pairs.append(
                {
                    "query": var_rel,
                    "target": orig_rel,
                    "variant": tag,
                    "genre": row["genre"],
                    "artist": row["artist"],
                    "track_id": row["track_id"],
                }
            )

    pairs_path = os.path.join(args.out, "pairs.csv")
    with open(pairs_path, "w", newline="") as f:
        w = csv.DictWriter(
            f, fieldnames=["query", "target", "variant", "genre", "artist", "track_id"]
        )
        w.writeheader()
        w.writerows(pairs)

    meta = {
        "n_sources": len(selected),
        "n_pairs": len(pairs),
        "n_failed": len(failed),
        "variants": [v[0] for v in VARIANTS],
        "out": os.path.abspath(args.out),
    }
    with open(os.path.join(args.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    if failed:
        with open(os.path.join(args.out, "failed.json"), "w") as f:
            json.dump(failed, f, indent=2)
            f.write("\n")

    print(json.dumps(meta, indent=2))
    return 0 if pairs else 1


if __name__ == "__main__":
    sys.exit(main())
