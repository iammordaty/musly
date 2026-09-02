#!/usr/bin/env python3
"""Score perturbation robustness from a Musly sparse dump and pairs.csv."""

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


def summarize(top1: List[float], rr: List[float], ranks: List[int]) -> dict:
    return {
        "n": len(top1),
        "top1": float(np.mean(top1)) if top1 else float("nan"),
        "mrr": float(np.mean(rr)) if rr else float("nan"),
        "median_rank": float(np.median(ranks)) if ranks else float("nan"),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--out-json", required=True)
    args = ap.parse_args()

    rankings = load_sparse_mirex(args.dump)
    pairs = list(csv.DictReader(open(args.pairs)))

    by_variant: Dict[str, dict] = defaultdict(lambda: {"top1": [], "rr": [], "ranks": []})
    overall = {"top1": [], "rr": [], "ranks": []}

    for row in pairs:
        neighs = find_ranking(rankings, row["query"])
        if neighs is None:
            continue
        target = row["target"]
        tag = row["variant"]
        rank = None
        for i, (npath, _d) in enumerate(neighs, start=1):
            if match_target(npath, target):
                rank = i
                break
        top1 = 1.0 if rank == 1 else 0.0
        rr = 0.0 if rank is None else 1.0 / float(rank)
        by_variant[tag]["top1"].append(top1)
        by_variant[tag]["rr"].append(rr)
        overall["top1"].append(top1)
        overall["rr"].append(rr)
        if rank is not None:
            by_variant[tag]["ranks"].append(rank)
            overall["ranks"].append(rank)

    out = {
        "overall": summarize(overall["top1"], overall["rr"], overall["ranks"]),
        "by_variant": {
            k: summarize(v["top1"], v["rr"], v["ranks"])
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
