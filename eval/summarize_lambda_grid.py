#!/usr/bin/env python3
"""Summarize the lambda x CSLS grid (tuning round 2) against an in-run baseline.

Historical record: the arms below are no longer registered methods. Round 2
merged lambda=0.15 into timbre2 and removed the CSLS path, so this script only
reads the stored results of that round (eval/results/lambda_20260903T071915Z).

The baseline is an arm of the same run, not the pinned one: the resampler
clipping fix changed the features, so cross-run comparison would conflate two
changes.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, Optional

# (arm, lambda, csls) — layout of the 4x2 design.
GRID = [
    ("timbre2", 0.10, False),
    ("timbre2_cs", 0.10, True),
    ("timbre2_sh15", 0.15, False),
    ("timbre2_cs_sh15", 0.15, True),
    ("timbre2_sh20", 0.20, False),
    ("timbre2_cs_sh20", 0.20, True),
    ("timbre2_sh25", 0.25, False),
    ("timbre2_cs_sh25", 0.25, True),
]


def load(path: str) -> Optional[dict]:
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return json.load(f)


def fmt(v, spec: str, dash: str = "—") -> str:
    if v is None:
        return f"{dash:>{len(f'{0:{spec}}')}}"
    return f"{v:{spec}}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", required=True, help="Ranking run directory")
    ap.add_argument("--perturbations", required=True, help="Perturbation output dir")
    ap.add_argument("--base", default="timbre2", help="Baseline arm inside --run")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    base = load(os.path.join(args.run, args.base, "metrics.json"))
    if base is None:
        raise SystemExit(f"Missing baseline metrics for arm {args.base} in {args.run}")

    rows = []
    for arm, lam, csls in GRID:
        m = load(os.path.join(args.run, arm, "metrics.json"))
        if m is None:
            continue
        pert = load(
            os.path.join(args.perturbations, arm, "perturbation_metrics.json")
        ) or {}
        cmp_json = load(
            os.path.join(args.run, f"compare_{args.base}_vs_{arm}.json")
        ) or {}
        ci = cmp_json.get("cluster_bootstrap_ci95") or [None, None]
        by_var = pert.get("by_variant", {})
        rows.append({
            "arm": arm,
            "lambda": lam,
            "csls": csls,
            "P@10": m.get("P@10"),
            "dP@10": m.get("P@10", 0.0) - base.get("P@10", 0.0),
            "ci95_P@10": ci,
            "wilcoxon_p": cmp_json.get("wilcoxon_p"),
            "hub_skew": m.get("hubness_skewness_k5"),
            "d_hub_skew": m.get("hubness_skewness_k5", 0.0)
            - base.get("hubness_skewness_k5", 0.0),
            "hub_max": m.get("hubness_max_k5"),
            "knn@5": m.get("knn_acc@5"),
            "auc": m.get("auc_same_genre"),
            "pert_top1": pert.get("overall", {}).get("top1"),
            "pert_mp3_64k": by_var.get("mp3_64k", {}).get("top1"),
            "pert_rate16k": by_var.get("rate16k", {}).get("top1"),
        })

    hdr = (f"{'arm':<16} {'lam':>5} {'csls':>5} {'P@10':>7} {'dP@10':>8} "
           f"{'hubskew':>8} {'dskew':>7} {'hubmax':>7} {'knn@5':>6} {'auc':>6} "
           f"{'pert':>6} {'mp3':>6} {'r16k':>6}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(
            f"{r['arm']:<16} {r['lambda']:>5.2f} {'yes' if r['csls'] else 'no':>5} "
            f"{fmt(r['P@10'], '7.4f')} {fmt(r['dP@10'], '+8.4f')} "
            f"{fmt(r['hub_skew'], '8.3f')} {fmt(r['d_hub_skew'], '+7.3f')} "
            f"{fmt(r['hub_max'], '7.0f')} {fmt(r['knn@5'], '6.3f')} "
            f"{fmt(r['auc'], '6.3f')} {fmt(r['pert_top1'], '6.3f')} "
            f"{fmt(r['pert_mp3_64k'], '6.3f')} {fmt(r['pert_rate16k'], '6.3f')}"
        )

    if args.out:
        with open(args.out, "w") as f:
            json.dump({"run": args.run, "base": args.base, "rows": rows}, f, indent=2)
            f.write("\n")
        print(f"\nWrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
