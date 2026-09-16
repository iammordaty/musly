#!/usr/bin/env python3
"""Build a small fixture set that separates clipping from gain sensitivity.

`eval/check_clipping.py` shows that 95 % of the `vol_p6` perturbations are
clipped, because FMA masters peak at roughly -0.1 dBFS and mp3 cannot carry a
+6 dB boost. So the original `vol_p6` number measures hard-clipping robustness,
not loudness robustness.

Three query groups over the same 200 originals separate the two effects:

  clip_p6  mp3,        +6 dB  — reproduces the clipped fixture
  f32_p6   float wav,  +6 dB  — same gain with headroom, no clipping
  f32_p0   float wav,   0 dB  — control for the container/codec change alone

If `f32_p6` scores like `f32_p0`, Musly's RMS normalization is gain invariant
and the weak `vol_p6` result was an artifact of the fixture.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import random
import shutil
import subprocess
import sys
from typing import List, Tuple

# (tag, extension, ffmpeg args after input)
GROUPS: List[Tuple[str, str, List[str]]] = [
    ("clip_p6", "mp3", ["-af", "volume=6dB", "-c:a", "libmp3lame"]),
    ("f32_p6", "wav", ["-af", "volume=6dB", "-c:a", "pcm_f32le"]),
    ("f32_p0", "wav", ["-c:a", "pcm_f32le"]),
]


def run_ffmpeg(job: Tuple[str, str, List[str]]) -> bool:
    src, dst, args = job
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    cmd = ["ffmpeg", "-y", "-v", "error", "-i", src, *args, dst]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and os.path.isfile(dst) and os.path.getsize(dst) > 0


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tree", default=os.path.join(here, "data", "tree"))
    ap.add_argument("--out", default=os.path.join(here, "data", "gaincheck"))
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=42, help="Same seed as make_perturbations.py")
    ap.add_argument("--jobs", type=int, default=8)
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

    jobs = []
    pairs = []
    for row in selected:
        rel = row["path"]
        real = os.path.realpath(os.path.join(args.tree, rel))
        if not os.path.isfile(real):
            continue
        orig_rel = os.path.join("originals", rel)
        orig_dst = os.path.join(args.out, orig_rel)
        os.makedirs(os.path.dirname(orig_dst), exist_ok=True)
        shutil.copy2(real, orig_dst)

        stem = os.path.splitext(rel)[0]
        for tag, ext, ff in GROUPS:
            var_rel = os.path.join("variants", tag, f"{stem}.{ext}")
            jobs.append((orig_dst, os.path.join(args.out, var_rel), ff))
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

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(run_ffmpeg, jobs))

    kept = [p for p, ok in zip(pairs, results) if ok]
    pairs_path = os.path.join(args.out, "pairs.csv")
    with open(pairs_path, "w", newline="") as f:
        w = csv.DictWriter(
            f, fieldnames=["query", "target", "variant", "genre", "artist", "track_id"]
        )
        w.writeheader()
        w.writerows(kept)

    meta = {
        "n_sources": len(selected),
        "n_pairs": len(kept),
        "n_failed": len(pairs) - len(kept),
        "groups": [g[0] for g in GROUPS],
        "out": os.path.abspath(args.out),
    }
    with open(os.path.join(args.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(json.dumps(meta, indent=2))
    return 0 if kept else 1


if __name__ == "__main__":
    sys.exit(main())
