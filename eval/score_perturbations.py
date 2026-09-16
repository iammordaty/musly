#!/usr/bin/env python3
"""Score perturbation robustness from a Musly sparse dump and pairs.csv.

The collection holds every variant of every source track, so the other
variants of the *same* track compete with the original for the top rank. Two
near-identical variants therefore push the original down and the score reads
as a robustness failure that is really an artifact of the fixture set. By
default those siblings are removed from each ranking before the original's
rank is taken; `--include-siblings` restores the old, confounded behaviour for
comparison.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from typing import Dict, List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from metrics import load_sparse_mirex


def find_ranking(rankings: dict, query: str) -> Optional[list]:
    if query in rankings:
        return rankings[query]
    for key, neighs in rankings.items():
        if key.endswith(query) or query.endswith(key):
            return neighs
    return None


def match_target(npath: str, target: str) -> bool:
    return npath == target or npath.endswith(target) or target.endswith(npath)


def source_key(path: str) -> str:
    """Identify the source track: genre/artist/id without prefix or extension."""
    parts = path.split("/")
    if "variants" in parts:
        i = parts.index("variants")
        parts = parts[i + 2:]  # drop "variants" and the tag
    elif "originals" in parts:
        i = parts.index("originals")
        parts = parts[i + 1:]
    return os.path.splitext("/".join(parts))[0]


def target_rank(
        neighs: list,
        target: str,
        key: str,
        drop_siblings: bool,
) -> tuple:
    """Rank of the original, and whether a sibling variant preceded it."""
    pos = 0
    sibling_ahead = 0.0
    for npath, _d in neighs:
        if match_target(npath, target):
            return pos + 1, sibling_ahead
        if source_key(npath) == key:
            if pos == 0:
                sibling_ahead = 1.0
            if drop_siblings:
                continue
        pos += 1
    return None, sibling_ahead


def summarize(
        top1: List[float],
        rr: List[float],
        ranks: List[int],
        sib: Optional[List[float]] = None,
) -> dict:
    out = {
        "n": len(top1),
        "top1": float(np.mean(top1)) if top1 else float("nan"),
        "mrr": float(np.mean(rr)) if rr else float("nan"),
        "median_rank": float(np.median(ranks)) if ranks else float("nan"),
    }
    if sib:
        out["sibling_ahead"] = float(np.mean(sib))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument(
        "--include-siblings",
        action="store_true",
        help="Let other variants of the same track compete (old behaviour)",
    )
    args = ap.parse_args()

    drop_siblings = not args.include_siblings
    rankings = load_sparse_mirex(args.dump)
    pairs = list(csv.DictReader(open(args.pairs)))

    empty = lambda: {"top1": [], "rr": [], "ranks": [], "sib": []}
    by_variant: Dict[str, dict] = defaultdict(empty)
    overall = empty()

    for row in pairs:
        neighs = find_ranking(rankings, row["query"])
        if neighs is None:
            continue
        target = row["target"]
        tag = row["variant"]
        rank, sibling_ahead = target_rank(
            neighs, target, source_key(row["query"]), drop_siblings
        )
        top1 = 1.0 if rank == 1 else 0.0
        rr = 0.0 if rank is None else 1.0 / float(rank)
        for acc in (by_variant[tag], overall):
            acc["top1"].append(top1)
            acc["rr"].append(rr)
            acc["sib"].append(sibling_ahead)
            if rank is not None:
                acc["ranks"].append(rank)

    out = {
        "siblings_excluded": drop_siblings,
        "overall": summarize(
            overall["top1"], overall["rr"], overall["ranks"], overall["sib"]
        ),
        "by_variant": {
            k: summarize(v["top1"], v["rr"], v["ranks"], v["sib"])
            for k, v in sorted(by_variant.items())
        },
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.out_json)) or ".", exist_ok=True)
    with open(args.out_json, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
