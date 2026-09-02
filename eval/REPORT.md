# Similarity validation report

This document records the validation harness under `eval/` and how to
interpret `timbre` vs `timbre2` results. It does **not** treat a different
neighbor list as proof of improvement.

## Scope of what this experiment can show

| Claim | Covered? |
|-------|----------|
| MFCC deltas (`timbre2` vs `timbre` on the same 0.3 code) | Yes |
| RMS normalization, log floor, silence gating, Ledoit-Wolf / double | Only via the **perturbation** suite (not via genre ranking between methods, because both methods share those stages) |
| Multi-segment analysis of long PCM (`quality-excerpt`) | Partially — only the `loop4m` perturbation exercises PCM longer than four minutes; FMA-small clips are 30 s |
| Upstream Musly 0.2 vs current code | **No** — no baseline arm from upstream was built (by design) |

## Ground truth

- **Primary:** FMA-small `genre_top` with binary relevance (same genre).
- **Mandatory artist filter** (`musly -s -f 1` after building
  `genre/artist/track.mp3` paths) so album/artist leakage does not inflate scores.
- **Perturbations:** for each variant query, the matching original is the sole
  relevant item (automatic, no annotation).

## Metrics

From each sparse dump (`k=100`, artist-filtered):

- Precision@K for K ∈ {1,5,10,20} — primary: **P@10**
- Recall@K, MAP@100, MRR, NDCG@10 / NDCG@100
- kNN genre accuracy @5 (majority vote)
- Hubness: skewness of k-occurrence (k=5)
- AUC (same-genre pairs) from a stratified 2000-track full matrix

Paired comparison (`compare.py`):

- Wilcoxon signed-rank on per-query deltas
- Cluster bootstrap over **artists** (10 000 resamples, seed 42), 95% CI
- Overlap@10 (Jaccard) and Kendall τ-b@10
- Per-genre mean delta; disagreement CSV for optional odd-one-out annotation

## Decision rules

Call **real improvement** of B over A only if all hold:

1. Wilcoxon p < 0.01  
2. Cluster-bootstrap 95% CI for ΔP@10 entirely above 0  
3. Same sign of Δ for P@1, P@5, P@10, P@20  
4. Hubness skewness does not increase materially  
5. Perturbation top-1 / MRR does not regress (especially `vol_*`)

If |ΔP@10| < 0.01, report **neutral** regardless of p-value (large N makes
tiny changes “significant”).

Low overlap@10 with a CI that includes 0 ⇒ **ranking changed, quality unclear**.

## How to run

```bash
# Dependencies
python3 -m venv eval/.venv
eval/.venv/bin/pip install -r eval/requirements.txt

# Data (~7.2 GiB audio + metadata)
eval/.venv/bin/python eval/fetch_fma.py --data-dir eval/data
# Rebuild the genre/artist tree (required again if an older tree has broken
# names like "Name_ 42014, dtype_ str" — that was a pandas MultiIndex bug).
rm -rf eval/data/tree
eval/.venv/bin/python eval/prepare_dataset.py --data-dir eval/data \
  --out eval/data/tree

# Build musly (Docker is the supported path), then benchmark both methods.
# run_benchmark.sh picks up image musly:dev automatically — no local build/ needed.
# First "-a" prints "Read 0 musly tracks" (empty collection); then Analyzing lines.
# Full FMA-small (~8000 tracks × 2 methods) takes a long time.
docker build -t musly:dev .
CHECK_DETERMINISM=1 eval/run_benchmark.sh

# Optional robustness set
eval/.venv/bin/python eval/make_perturbations.py --tree eval/data/tree --n 200
eval/run_perturbations.sh timbre
eval/run_perturbations.sh timbre2

# Level-2 regression against the pinned baseline
eval/.venv/bin/python eval/regression.py \
  --metrics eval/results/<run>/timbre2/metrics.json \
  --run-json eval/results/<run>/timbre2/run.json \
  --baseline eval/baselines/fma_small.json
```

The pinned baseline lives in `eval/baselines/fma_small.json` (run
`fma_small_20260831T103326Z`). Re-pin only after an intentional accepted
change: copy fresh `metrics.json` values, set `environment.manifest_sha256`
from `run.json` / `manifest.json`, and refresh the `ci` bands if needed.

## Results

Run: `eval/results/fma_small_20260831T103326Z`  
Dataset: FMA-small, 7994 analyzed / 8000 prepared, artist filter on, k=100  
Manifest: `6bf64128a580fe4d54b32d9e3d410b2f028126a055b4e3a88e6d3f65ee31a3ec`

### Genre ranking (artist-filtered)

| Method | P@1 | P@5 | P@10 | P@20 | MAP@100 | knn@5 | hubness skew | AUC |
|--------|-----|-----|------|------|---------|-------|--------------|-----|
| timbre | 0.377 | 0.357 | 0.347 | 0.334 | 0.0153 | 0.425 | 2.23 | 0.581 |
| timbre2 | 0.391 | 0.373 | 0.360 | 0.345 | 0.0160 | 0.443 | 2.76 | 0.579 |

Paired `timbre` → `timbre2` on P@10 (`compare.json`):

- ΔP@10 = **+0.0132** (wins 2598 / losses 2002 / ties 3394)
- Wilcoxon p ≈ 4.2×10⁻¹⁸
- Cluster-bootstrap 95% CI: **[0.0094, 0.0167]** (entirely above 0)
- Same sign for ΔP@1 / ΔP@5 / ΔP@10 / ΔP@20 (all positive)
- Overlap@10 ≈ 0.50, Kendall τ-b@10 ≈ 0.24 (rankings changed, not identical)
- Per-genre: gains on Folk, Hip-Hop, Rock, International, Pop; small losses on Electronic and Instrumental

### Perturbation robustness (200 sources × 8 variants)

| Method | overall top-1 | vol_m12 | vol_p6 | mp3_64k | lowpass8k | rate16k | silence_pre20 | mid15s | loop4m |
|--------|---------------|---------|--------|---------|-----------|---------|---------------|--------|--------|
| timbre | 0.333 | 0.60 | 0.11 | 0.12 | 0.84 | 0.00 | 0.12 | 0.49 | 0.39 |
| timbre2 | 0.336 | 0.60 | 0.10 | 0.11 | 0.83 | 0.00 | 0.14 | 0.50 | 0.42 |

Overall top-1 / MRR are essentially tied. Volume variants are **not** near
1.0 for either method (especially `vol_p6`), so the RMS-normalization claim
is only partially supported. `rate16k` fails for both.

### Verdict

Against the written decision rules:

1–3 (ranking significance / CI / consistent P@K sign): **pass** for `timbre2`
4 (hubness): **fail** — skewness rose 2.23 → 2.76, max k-occurrence 69 → 96  
5 (perturbations): **pass vs each other** (no regression), but absolute
   volume robustness is weak for **both** methods

**Conclusion:** MFCC deltas improve artist-filtered genre ranking by a small
but statistically solid margin (≈ +1.3 pp P@10). That is not a blanket
quality win: hubness got worse, and stage-3 robustness (shared by both
methods) still looks incomplete on gain changes and resampling. Keep
`timbre2` as default for retrieval-oriented use, watch hubness on large
collections, and treat volume / sample-rate robustness as open follow-up —
not as evidence that stages 2–3 are fully validated.

Manual odd-one-out annotation is optional; automatic metrics are already
decisive on ranking, and the main open questions are hubness and absolute
perturbation floors rather than ties between the two methods.

## Implementation notes tied to Musly

- `tracks_add` sorts paths and appends in index order so collection order (and
  Mutual Proximity) is reproducible under OpenMP.
- `musly -s -f NUM` applies the artist filter **while** selecting neighbors
  (not after truncating to k).
- Sparse dump lines are buffered per query index so OpenMP does not scramble
  order (needed for dump SHA-256 determinism checks).
