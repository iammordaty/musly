#!/usr/bin/env python3
"""Compute ranking metrics from a Musly MIREX sparse dump (and optional full matrix).

Expects collection paths of the form genre/artist/track.mp3 (as produced by
prepare_dataset.py). Artist filtering should already have been applied when
the dump was written with musly -s -f 1.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from collections import Counter, defaultdict
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np


def parse_path_labels(path: str) -> Tuple[str, str]:
    parts = path.strip().split("/")
    # Tolerate leading "./" or absolute prefixes: use the last three components.
    if len(parts) < 3:
        return "Unknown", "Unknown"
    return parts[-3], parts[-2]


def load_sparse_mirex(path: str) -> Dict[str, List[Tuple[str, float]]]:
    """Return query -> ordered list of (neighbor_path, distance)."""
    rankings: Dict[str, List[Tuple[str, float]]] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline()
        if not header:
            raise SystemExit(f"Empty dump: {path}")
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            cols = line.split("\t")
            query = cols[0]
            neighbors = []
            for cell in cols[1:]:
                if not cell:
                    continue
                # filename,distance — filename may contain commas rarely; split from right
                if "," not in cell:
                    continue
                name, dist_s = cell.rsplit(",", 1)
                try:
                    dist = float(dist_s)
                except ValueError:
                    continue
                neighbors.append((name, dist))
            rankings[query] = neighbors
    return rankings


def load_full_mirex(path: str) -> Tuple[List[str], np.ndarray]:
    """Parse a Musly full MIREX matrix into (paths, distance matrix)."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        _header = f.readline()
        paths: List[str] = []
        while True:
            pos = f.tell()
            line = f.readline()
            if not line:
                break
            if line.startswith("Q/R"):
                break
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 2 and cols[0].isdigit():
                paths.append(cols[1])
            else:
                f.seek(pos)
                break
        n = len(paths)
        mat = np.full((n, n), np.nan, dtype=np.float64)
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if not cols or not cols[0].isdigit():
                continue
            i = int(cols[0]) - 1
            vals = [float(x) for x in cols[1 : n + 1]]
            if len(vals) == n:
                mat[i, :] = vals
    return paths, mat


def precision_at_k(rels: Sequence[bool], k: int) -> float:
    if k <= 0:
        return 0.0
    top = rels[:k]
    if not top:
        return 0.0
    return sum(1 for r in top if r) / float(len(top))


def recall_at_k(rels: Sequence[bool], k: int, n_relevant: int) -> float:
    if n_relevant <= 0:
        return 0.0
    return sum(1 for r in rels[:k] if r) / float(n_relevant)


def average_precision(rels: Sequence[bool], n_relevant: int) -> float:
    if n_relevant <= 0:
        return 0.0
    hit = 0
    s = 0.0
    for i, r in enumerate(rels, start=1):
        if r:
            hit += 1
            s += hit / float(i)
    return s / float(n_relevant)


def dcg_at_k(rels: Sequence[bool], k: int) -> float:
    s = 0.0
    for i, r in enumerate(rels[:k], start=1):
        if r:
            s += 1.0 / math.log2(i + 1)
    return s


def ndcg_at_k(rels: Sequence[bool], k: int, n_relevant: int) -> float:
    ideal_hits = min(k, max(0, n_relevant))
    if ideal_hits <= 0:
        return 0.0
    ideal = [True] * ideal_hits + [False] * max(0, k - ideal_hits)
    idcg = dcg_at_k(ideal, k)
    if idcg <= 0:
        return 0.0
    return dcg_at_k(rels, k) / idcg


def first_relevant_rank(rels: Sequence[bool]) -> Optional[int]:
    for i, r in enumerate(rels, start=1):
        if r:
            return i
    return None


def majority_genre(neighbor_genres: Sequence[str]) -> Optional[str]:
    if not neighbor_genres:
        return None
    counts = Counter(neighbor_genres)
    # Tie-break: first max in encounter order (stable Counter most_common).
    return counts.most_common(1)[0][0]


def skewness(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64)
    if x.size < 3:
        return 0.0
    m = x.mean()
    s = x.std(ddof=0)
    if s == 0:
        return 0.0
    return float(np.mean(((x - m) / s) ** 3))


def auc_from_scores(pos: np.ndarray, neg: np.ndarray) -> float:
    """AUC for higher-score-is-better. We pass negated distances."""
    if pos.size == 0 or neg.size == 0:
        return float("nan")
    scores = np.concatenate([pos, neg])
    labels = np.concatenate([np.ones(pos.size), np.zeros(neg.size)])
    order = np.argsort(scores)
    ranks = np.empty_like(order, dtype=np.float64)
    ranks[order] = np.arange(1, len(scores) + 1)
    # Average ranks for ties
    # Simple midrank via scipy would be nicer; do a light pass:
    sorted_scores = scores[order]
    i = 0
    while i < len(sorted_scores):
        j = i
        while j + 1 < len(sorted_scores) and sorted_scores[j + 1] == sorted_scores[i]:
            j += 1
        if j > i:
            mid = 0.5 * (i + j) + 1.0
            ranks[order[i : j + 1]] = mid
        i = j + 1
    n_pos = float(pos.size)
    n_neg = float(neg.size)
    sum_ranks_pos = ranks[labels == 1].sum()
    return float((sum_ranks_pos - n_pos * (n_pos + 1) / 2.0) / (n_pos * n_neg))


def evaluate(
    rankings: Dict[str, List[Tuple[str, float]]],
    full_matrix: Optional[Tuple[List[str], np.ndarray]] = None,
    knn_k: int = 5,
) -> Tuple[dict, List[dict]]:
    labels = {q: parse_path_labels(q) for q in rankings}
    for q, neighs in rankings.items():
        for n, _ in neighs:
            if n not in labels:
                labels[n] = parse_path_labels(n)

    # Relevant pool size per query: same genre, other artist, in the collection.
    all_paths = list(rankings.keys())
    genre_of = {p: labels[p][0] for p in all_paths}
    artist_of = {p: labels[p][1] for p in all_paths}

    by_genre: Dict[str, List[str]] = defaultdict(list)
    for p in all_paths:
        by_genre[genre_of[p]].append(p)

    per_query = []
    hub_counts = Counter()
    knn_correct = 0
    knn_total = 0

    ks = (1, 5, 10, 20)
    agg = {f"P@{k}": [] for k in ks}
    agg.update({f"R@{k}": [] for k in ks})
    agg["AP"] = []
    agg["NDCG@10"] = []
    agg["NDCG@100"] = []
    agg["RR"] = []

    for query, neighbors in rankings.items():
        gq, aq = genre_of[query], artist_of[query]
        # Relevant = same genre, different artist (artist filter already applied
        # in the dump, but re-check for safety).
        relevant_pool = [
            p
            for p in by_genre[gq]
            if p != query and artist_of[p] != aq
        ]
        n_rel = len(relevant_pool)
        rel_set = set(relevant_pool)

        rels = []
        neigh_genres = []
        for npath, _dist in neighbors:
            is_rel = npath in rel_set
            rels.append(is_rel)
            neigh_genres.append(genre_of.get(npath, "Unknown"))
            # hubness from top-5
        for npath, _ in neighbors[:knn_k]:
            hub_counts[npath] += 1

        row = {
            "query": query,
            "genre": gq,
            "artist": aq,
            "n_relevant": n_rel,
        }
        for k in ks:
            p = precision_at_k(rels, k)
            r = recall_at_k(rels, k, n_rel)
            row[f"P@{k}"] = p
            row[f"R@{k}"] = r
            agg[f"P@{k}"].append(p)
            agg[f"R@{k}"].append(r)

        ap = average_precision(rels, n_rel)
        nd10 = ndcg_at_k(rels, 10, n_rel)
        nd100 = ndcg_at_k(rels, 100, n_rel)
        rank1 = first_relevant_rank(rels)
        rr = 0.0 if rank1 is None else 1.0 / float(rank1)
        row["AP"] = ap
        row["NDCG@10"] = nd10
        row["NDCG@100"] = nd100
        row["RR"] = rr
        row["first_relevant_rank"] = rank1 if rank1 is not None else ""
        agg["AP"].append(ap)
        agg["NDCG@10"].append(nd10)
        agg["NDCG@100"].append(nd100)
        agg["RR"].append(rr)

        pred = majority_genre(neigh_genres[:knn_k])
        if pred is not None:
            knn_total += 1
            if pred == gq:
                knn_correct += 1
        per_query.append(row)

    metrics = {
        key: float(np.mean(vals)) if vals else float("nan") for key, vals in agg.items()
    }
    metrics["MAP@100"] = metrics.pop("AP")
    metrics["MRR"] = metrics.pop("RR")
    metrics["knn_acc@5"] = (knn_correct / knn_total) if knn_total else float("nan")
    metrics["n_queries"] = len(per_query)

    # Hubness: occurrence counts for items that appeared in any top-k; include zeros
    # for tracks never retrieved to avoid optimistic skew.
    occ = np.array([hub_counts.get(p, 0) for p in all_paths], dtype=np.float64)
    metrics["hubness_skewness_k5"] = skewness(occ)
    metrics["hubness_max_k5"] = float(occ.max()) if occ.size else 0.0

    if full_matrix is not None:
        paths, mat = full_matrix
        idx = {p: i for i, p in enumerate(paths)}
        pos_scores = []
        neg_scores = []
        # Sample pairs: for each query in the subset, use all others.
        for p in paths:
            i = idx[p]
            gp, ap = parse_path_labels(p)
            for q in paths:
                if p == q:
                    continue
                j = idx[q]
                gq, aq = parse_path_labels(q)
                if ap == aq:
                    continue  # artist filter
                d = mat[i, j]
                if not np.isfinite(d):
                    continue
                score = -float(d)  # smaller distance → higher score
                if gp == gq:
                    pos_scores.append(score)
                else:
                    neg_scores.append(score)
        metrics["auc_same_genre"] = auc_from_scores(
            np.asarray(pos_scores, dtype=np.float64),
            np.asarray(neg_scores, dtype=np.float64),
        )
        metrics["auc_n_pos"] = len(pos_scores)
        metrics["auc_n_neg"] = len(neg_scores)

    return metrics, per_query


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dump", required=True, help="MIREX sparse dump from musly -s")
    ap.add_argument("--full", default="", help="Optional full MIREX matrix for AUC")
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--knn-k", type=int, default=5)
    args = ap.parse_args()

    rankings = load_sparse_mirex(args.dump)
    full = load_full_mirex(args.full) if args.full else None
    metrics, per_query = evaluate(rankings, full, knn_k=args.knn_k)

    os.makedirs(os.path.dirname(os.path.abspath(args.out_json)) or ".", exist_ok=True)
    with open(args.out_json, "w") as f:
        json.dump(metrics, f, indent=2)
        f.write("\n")

    fieldnames = list(per_query[0].keys()) if per_query else [
        "query",
        "genre",
        "artist",
        "n_relevant",
        "P@1",
        "P@5",
        "P@10",
        "P@20",
        "R@1",
        "R@5",
        "R@10",
        "R@20",
        "AP",
        "NDCG@10",
        "NDCG@100",
        "RR",
        "first_relevant_rank",
    ]
    with open(args.out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(per_query)

    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
