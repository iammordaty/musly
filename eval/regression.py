#!/usr/bin/env python3
"""Compare fresh metrics.json against a pinned baseline (level-2 regression)."""

from __future__ import annotations

import argparse
import json
import os
import sys


def load_json(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--metrics", required=True, help="Fresh metrics.json")
    ap.add_argument(
        "--run-json",
        default="",
        help="Optional run.json with environment fingerprint (manifest_sha256, method)",
    )
    ap.add_argument(
        "--baseline",
        default=os.path.join(os.path.dirname(__file__), "baselines", "fma_small.json"),
    )
    ap.add_argument(
        "--p10-drop-tol",
        type=float,
        default=0.005,
        help="Fail if P@10 drops by more than this absolute amount",
    )
    args = ap.parse_args()

    metrics = load_json(args.metrics)
    if not os.path.isfile(args.baseline):
        print(f"No baseline at {args.baseline}; nothing to compare.", file=sys.stderr)
        return 0

    baseline = load_json(args.baseline)
    env = baseline.get("environment", {})

    # Fingerprint: prefer --run-json; else accept nested metrics["environment"].
    fresh_env = dict(metrics.get("environment") or {})
    if args.run_json:
        run = load_json(args.run_json)
        for key in ("manifest_sha256", "method", "k", "n_analyzed"):
            if key in run:
                fresh_env[key] = run[key]

    for key in ("manifest_sha256", "method"):
        if key in env and key in fresh_env and env[key] != fresh_env[key]:
            print(
                f"Baseline environment mismatch on {key}: "
                f"{env[key]} vs {fresh_env[key]}. Skipping fail.",
                file=sys.stderr,
            )
            return 0

    if env.get("manifest_sha256") and "manifest_sha256" not in fresh_env:
        print(
            "No manifest fingerprint on the fresh side "
            "(pass --run-json). Skipping fail.",
            file=sys.stderr,
        )
        return 0

    failures = []
    base_p10 = float(baseline["metrics"]["P@10"])
    fresh_p10 = float(metrics["P@10"])
    if fresh_p10 < base_p10 - args.p10_drop_tol:
        failures.append(
            f"P@10 dropped: {fresh_p10:.4f} < {base_p10:.4f} - {args.p10_drop_tol}"
        )

    for name, bound in baseline.get("ci", {}).items():
        if name not in metrics:
            continue
        lo, hi = bound
        val = float(metrics[name])
        if val < lo or val > hi:
            failures.append(f"{name}={val:.4f} outside pinned CI [{lo}, {hi}]")

    if failures:
        print("REGRESSION:", file=sys.stderr)
        for fmsg in failures:
            print(f"  - {fmsg}", file=sys.stderr)
        return 1

    print("OK: metrics within baseline tolerances")
    return 0


if __name__ == "__main__":
    sys.exit(main())
