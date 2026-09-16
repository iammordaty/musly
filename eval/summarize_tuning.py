#!/usr/bin/env python3
"""Compare tuning run metrics against the pinned timbre2 baseline."""

from __future__ import annotations

import argparse
import json
import os
import sys


def load(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--baseline-run",
        default="eval/results/fma_small_20260831T103326Z",
        help="Baseline harness run directory",
    )
    ap.add_argument("--tuning-run", required=True, help="New tuning run directory")
    ap.add_argument("--out", default="", help="Optional JSON summary path")
    args = ap.parse_args()

    base_m = load(os.path.join(args.baseline_run, "timbre2/metrics.json"))
    base_p = load(
        os.path.join(args.baseline_run, "../perturbations/timbre2/perturbation_metrics.json")
    )
    if not os.path.isfile(
        os.path.join(args.baseline_run, "../perturbations/timbre2/perturbation_metrics.json")
    ):
        base_p = load("eval/results/perturbations/timbre2/perturbation_metrics.json")

    # run_benchmark.sh nests its output under <run>/fma_small_<stamp>/<method>,
    # while the clone path writes <run>/<method> directly.
    method_dirs = {}
    for root, dirs, files in os.walk(args.tuning_run):
        if "metrics.json" in files:
            method_dirs.setdefault(os.path.basename(root), root)

    rows = []
    for method in sorted(method_dirs):
        mdir = method_dirs[method]
        m = load(os.path.join(mdir, "metrics.json"))
        pert = {}
        pf = os.path.join("eval/results/perturbations", method, "perturbation_metrics.json")
        if os.path.isfile(pf):
            pert = load(pf)
        row = {
            "method": method,
            "P@10": m.get("P@10"),
            "dP@10": m.get("P@10", 0) - base_m.get("P@10", 0),
            "hubness": m.get("hubness_skewness_k5"),
            "d_hubness": m.get("hubness_skewness_k5", 0)
            - base_m.get("hubness_skewness_k5", 0),
            "hub_max": m.get("hubness_max_k5"),
            "d_hub_max": m.get("hubness_max_k5", 0) - base_m.get("hubness_max_k5", 0),
            "knn@5": m.get("knn_acc@5"),
            "auc": m.get("auc_same_genre"),
            "pert_top1": pert.get("overall", {}).get("top1"),
            "pert_vol_m12": pert.get("by_variant", {}).get("vol_m12", {}).get("top1"),
            "pert_vol_p6": pert.get("by_variant", {}).get("vol_p6", {}).get("top1"),
        }
        rows.append(row)

    print(f"Baseline timbre2: P@10={base_m['P@10']:.4f} hubness={base_m['hubness_skewness_k5']:.3f} "
          f"pert_top1={base_p.get('overall',{}).get('top1', 'n/a')}")
    print(
        f"{'method':<14} {'P@10':>7} {'dP@10':>7} {'hub':>6} {'dhub':>7} "
        f"{'hubmax':>7} {'knn@5':>6} {'auc':>6} {'pert':>6} {'vol12':>6} {'vol6':>6}"
    )
    for r in rows:
        print(
            f"{r['method']:<14} "
            f"{r['P@10']:7.4f} {r['dP@10']:+7.4f} "
            f"{r['hubness']:6.2f} {r['d_hubness']:+7.3f} "
            f"{(r['hub_max'] or 0):7.0f} "
            f"{(r['knn@5'] or 0):6.3f} "
            f"{(r['auc'] or 0):6.3f} "
            f"{(r['pert_top1'] or 0):6.3f} "
            f"{(r['pert_vol_m12'] or 0):6.3f} "
            f"{(r['pert_vol_p6'] or 0):6.3f}"
        )

    if args.out:
        with open(args.out, "w") as f:
            json.dump({"baseline_run": args.baseline_run, "rows": rows}, f, indent=2)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
