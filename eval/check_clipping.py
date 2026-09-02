#!/usr/bin/env python3
"""Measure clipping in the perturbation fixtures with ffmpeg volumedetect.

Answers one question: is the weak `vol_p6` robustness a Musly problem, or was
the +6 dB fixture already clipped by ffmpeg before Musly ever read it?

For every pair in pairs.csv the original and the variant are decoded and their
peak level compared. A variant is called clipped when its peak sits at the
0 dBFS ceiling while the expected peak (original peak + gain) is above it.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import re
import statistics
import subprocess
import sys
from typing import Dict, Optional

MAX_RE = re.compile(r"max_volume:\s*(-?[\d.]+) dB")
MEAN_RE = re.compile(r"mean_volume:\s*(-?[\d.]+) dB")
HIST0_RE = re.compile(r"histogram_0db:\s*(\d+)")

# Nominal gain applied by make_perturbations.py, in dB.
VARIANT_GAIN = {"vol_p6": 6.0, "vol_m12": -12.0}

# ffmpeg reports peaks in 0.1 dB steps, so anything within this distance of the
# ceiling counts as "at the ceiling".
CEILING_TOL_DB = 0.15


def volumedetect(path: str) -> Optional[Dict[str, float]]:
    cmd = [
        "ffmpeg", "-v", "info", "-nostats", "-i", path,
        "-af", "volumedetect", "-f", "null", "-",
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    err = r.stderr.decode("utf-8", "replace")
    m_max = MAX_RE.search(err)
    m_mean = MEAN_RE.search(err)
    if not m_max or not m_mean:
        return None
    m_hist = HIST0_RE.search(err)
    return {
        "max_db": float(m_max.group(1)),
        "mean_db": float(m_mean.group(1)),
        "hist_0db": float(m_hist.group(1)) if m_hist else 0.0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--pert",
        default=os.path.join(os.path.dirname(__file__), "data", "perturbations"),
    )
    ap.add_argument(
        "--variants",
        default="vol_p6,vol_m12",
        help="Comma separated variant tags to check",
    )
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out-json", default="")
    args = ap.parse_args()

    tags = [t for t in args.variants.split(",") if t]
    pairs_path = os.path.join(args.pert, "pairs.csv")
    if not os.path.isfile(pairs_path):
        raise SystemExit(f"Missing {pairs_path}")

    rows = [r for r in csv.DictReader(open(pairs_path)) if r["variant"] in tags]
    if not rows:
        raise SystemExit(f"No pairs for variants {tags}")

    paths = set()
    for r in rows:
        paths.add(os.path.join(args.pert, r["target"]))
        paths.add(os.path.join(args.pert, r["query"]))

    measured: Dict[str, Optional[Dict[str, float]]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for path, res in zip(paths, ex.map(volumedetect, paths)):
            measured[path] = res

    report = {}
    for tag in tags:
        gain = VARIANT_GAIN.get(tag, 0.0)
        n_clipped = 0
        n_ok = 0
        orig_max = []
        var_max = []
        headroom_lost = []
        hist0 = []
        for r in (x for x in rows if x["variant"] == tag):
            o = measured.get(os.path.join(args.pert, r["target"]))
            v = measured.get(os.path.join(args.pert, r["query"]))
            if not o or not v:
                continue
            expected = o["max_db"] + gain
            orig_max.append(o["max_db"])
            var_max.append(v["max_db"])
            hist0.append(v["hist_0db"])
            at_ceiling = v["max_db"] >= -CEILING_TOL_DB
            if at_ceiling and expected > CEILING_TOL_DB:
                n_clipped += 1
                headroom_lost.append(expected - v["max_db"])
            else:
                n_ok += 1
        n = n_clipped + n_ok
        report[tag] = {
            "n": n,
            "n_clipped": n_clipped,
            "frac_clipped": (n_clipped / n) if n else 0.0,
            "median_orig_max_db": statistics.median(orig_max) if orig_max else None,
            "median_variant_max_db": statistics.median(var_max) if var_max else None,
            "median_headroom_lost_db": (
                statistics.median(headroom_lost) if headroom_lost else None
            ),
            "median_hist_0db": statistics.median(hist0) if hist0 else None,
            "max_hist_0db": max(hist0) if hist0 else None,
        }

    for tag, d in report.items():
        print(f"--- {tag}  (n={d['n']}, nominal gain {VARIANT_GAIN.get(tag, 0.0):+g} dB)")
        print(f"  clipped:               {d['n_clipped']} / {d['n']} "
              f"({100.0 * d['frac_clipped']:.1f} %)")
        print(f"  median original peak:  {d['median_orig_max_db']} dB")
        print(f"  median variant peak:   {d['median_variant_max_db']} dB")
        print(f"  median headroom lost:  {d['median_headroom_lost_db']} dB")
        print(f"  samples at 0 dB:       median {d['median_hist_0db']}, "
              f"max {d['max_hist_0db']}")

    if args.out_json:
        with open(args.out_json, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
