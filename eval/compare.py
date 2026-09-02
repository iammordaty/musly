#!/usr/bin/env python3
"""Paired statistical comparison of two per_query.csv files (timbre vs timbre2)."""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from typing import Dict, List, Sequence, Tuple

import numpy as np
from scipy import stats

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load_per_query(path: str) -> Dict[str, dict]:
    rows = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows[row["query"]] = row
    return rows


def load_rankings(dump: str) -> Dict[str, List[str]]:
    from metrics import load_sparse_mirex

    raw = load_sparse_mirex(dump)
    return {q: [n for n, _ in neighs] for q, neighs in raw.items()}


def jaccard(a: Sequence[str], b: Sequence[str]) -> float:
    sa, sb = set(a), set(b)
    if not sa and not sb:
        return 1.0
    return len(sa & sb) / float(len(sa | sb))


def kendall_tau_b_from_lists(a: Sequence[str], b: Sequence[str], k: int = 10) -> float:
    """Kendall tau-b on the union of top-k lists (rank = position, missing = k+1)."""
    a = list(a[:k])
    b = list(b[:k])
    universe = list(dict.fromkeys(a + b))
    if len(universe) < 2:
        return 1.0
    ra = {x: i + 1 for i, x in enumerate(a)}
    rb = {x: i + 1 for i, x in enumerate(b)}
    default = k + 1
    x = np.array([ra.get(u, default) for u in universe], dtype=float)
    y = np.array([rb.get(u, default) for u in universe], dtype=float)
    if np.all(x == x[0]) or np.all(y == y[0]):
        return 1.0 if np.array_equal(x, y) else 0.0
    tau, _ = stats.kendalltau(x, y, variant="b")
    return float(tau) if tau == tau else 0.0


def cluster_bootstrap_ci(
    artists: Sequence[str],
    deltas: Sequence[float],
    n_boot: int = 10000,
    seed: int = 42,
    alpha: float = 0.05,
) -> Tuple[float, float, float]:
    """Cluster bootstrap resampling artists; returns (mean, lo, hi)."""
    by_artist: Dict[str, List[float]] = defaultdict(list)
    for a, d in zip(artists, deltas):
        by_artist[a].append(d)
    keys = list(by_artist.keys())
    rng = np.random.default_rng(seed)
    means = np.empty(n_boot, dtype=np.float64)
    for b in range(n_boot):
        chosen = rng.choice(keys, size=len(keys), replace=True)
        sample = []
        for k in chosen:
            sample.extend(by_artist[k])
        means[b] = np.mean(sample) if sample else 0.0
    lo = float(np.quantile(means, alpha / 2))
    hi = float(np.quantile(means, 1 - alpha / 2))
    return float(np.mean(deltas)), lo, hi


def cohens_d(deltas: np.ndarray) -> float:
    if deltas.size < 2:
        return 0.0
    s = deltas.std(ddof=1)
    if s == 0:
        return 0.0
    return float(deltas.mean() / s)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--a-csv", required=True, help="per_query.csv for method A (e.g. timbre)")
    ap.add_argument("--b-csv", required=True, help="per_query.csv for method B (e.g. timbre2)")
    ap.add_argument("--a-dump", default="", help="Optional sparse dump A for ranking overlap")
    ap.add_argument("--b-dump", default="", help="Optional sparse dump B for ranking overlap")
    ap.add_argument("--metric", default="P@10", help="Primary per-query metric")
    ap.add_argument("--n-boot", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-disagreements", default="")
    args = ap.parse_args()

    a_rows = load_per_query(args.a_csv)
    b_rows = load_per_query(args.b_csv)
    common = sorted(set(a_rows) & set(b_rows))
    if not common:
        raise SystemExit("No overlapping queries between the two CSVs")

    metric = args.metric
    deltas = []
    artists = []
    genres = []
    a_vals = []
    b_vals = []
    for q in common:
        av = float(a_rows[q][metric])
        bv = float(b_rows[q][metric])
        a_vals.append(av)
        b_vals.append(bv)
        deltas.append(bv - av)  # B minus A: positive ⇒ B better
        artists.append(a_rows[q]["artist"])
        genres.append(a_rows[q]["genre"])

    deltas_a = np.asarray(deltas, dtype=np.float64)
    a_vals_a = np.asarray(a_vals, dtype=np.float64)
    b_vals_a = np.asarray(b_vals, dtype=np.float64)

    # Wilcoxon signed-rank on paired differences (two-sided).
    nonzero = deltas_a[deltas_a != 0]
    if nonzero.size >= 1:
        w_stat, w_p = stats.wilcoxon(nonzero, alternative="two-sided", zero_method="wilcox")
    else:
        w_stat, w_p = 0.0, 1.0

    mean_d, ci_lo, ci_hi = cluster_bootstrap_ci(
        artists, deltas, n_boot=args.n_boot, seed=args.seed
    )

    wins = int(np.sum(deltas_a > 0))
    losses = int(np.sum(deltas_a < 0))
    ties = int(np.sum(deltas_a == 0))

    # Per-genre mean delta
    by_genre: Dict[str, List[float]] = defaultdict(list)
    for g, d in zip(genres, deltas):
        by_genre[g].append(d)
    genre_delta = {g: float(np.mean(v)) for g, v in sorted(by_genre.items())}

    result = {
        "n_queries": len(common),
        "metric": metric,
        "mean_A": float(a_vals_a.mean()),
        "mean_B": float(b_vals_a.mean()),
        "mean_delta_B_minus_A": mean_d,
        "cohens_d": cohens_d(deltas_a),
        "wilcoxon_stat": float(w_stat),
        "wilcoxon_p": float(w_p),
        "cluster_bootstrap_ci95": [ci_lo, ci_hi],
        "wins_B": wins,
        "losses_B": losses,
        "ties": ties,
        "delta_by_genre": genre_delta,
        "interpretation_hint": (
            "real_improvement"
            if (
                w_p < 0.01
                and ci_lo > 0
                and mean_d >= 0.01
            )
            else (
                "real_regression"
                if (w_p < 0.01 and ci_hi < 0 and mean_d <= -0.01)
                else (
                    "neutral_small_effect"
                    if abs(mean_d) < 0.01
                    else "ranking_change_inconclusive"
                )
            )
        ),
    }

    # Ranking agreement if dumps provided
    if args.a_dump and args.b_dump:
        ra = load_rankings(args.a_dump)
        rb = load_rankings(args.b_dump)
        overlaps = []
        taus = []
        for q in common:
            if q not in ra or q not in rb:
                continue
            overlaps.append(jaccard(ra[q][:10], rb[q][:10]))
            taus.append(kendall_tau_b_from_lists(ra[q], rb[q], k=10))
        result["overlap@10_mean"] = float(np.mean(overlaps)) if overlaps else float("nan")
        result["kendall_tau_b@10_mean"] = float(np.mean(taus)) if taus else float("nan")

    # Extra metrics sign consistency
    for m in ("P@1", "P@5", "P@10", "P@20"):
        if m == metric:
            continue
        if m not in a_rows[common[0]] or m not in b_rows[common[0]]:
            continue
        d = np.array(
            [float(b_rows[q][m]) - float(a_rows[q][m]) for q in common], dtype=np.float64
        )
        result[f"mean_delta_{m}"] = float(d.mean())

    os.makedirs(os.path.dirname(os.path.abspath(args.out_json)) or ".", exist_ok=True)
    with open(args.out_json, "w") as f:
        json.dump(result, f, indent=2)
        f.write("\n")

    if args.out_disagreements:
        # Largest absolute change in primary metric
        ranked = sorted(
            common, key=lambda q: abs(float(b_rows[q][metric]) - float(a_rows[q][metric])), reverse=True
        )
        with open(args.out_disagreements, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["query", "genre", "artist", f"A_{metric}", f"B_{metric}", "delta"])
            for q in ranked[:200]:
                av = float(a_rows[q][metric])
                bv = float(b_rows[q][metric])
                w.writerow([q, a_rows[q]["genre"], a_rows[q]["artist"], av, bv, bv - av])

    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
