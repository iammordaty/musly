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
  relevant item (automatic, no annotation). The other variants of the *same*
  source track are **excluded from the ranking** before the original's rank is
  taken — see the note under the perturbation results for why.

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
5. Perturbation top-1 / MRR does not regress (read the sibling-excluded
   numbers; on the corrected metric all methods sit within ~1 pp, so this rule
   discriminates only for `mp3_64k` and `rate16k`)

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

# Gain-invariance check (200 sources, ~7 min for both methods)
eval/.venv/bin/python eval/make_gain_fixtures.py
eval/run_gaincheck.sh

# Peak levels of the fixtures; use astats, not volumedetect
eval/.venv/bin/python eval/check_clipping.py

# Level-2 regression against the pinned baseline
eval/.venv/bin/python eval/regression.py \
  --metrics eval/results/<run>/timbre2/metrics.json \
  --run-json eval/results/<run>/timbre2/run.json \
  --baseline eval/baselines/fma_small.json
```

The pinned baseline lives in `eval/baselines/fma_small.json`, pinned from the
tuning round-2 winner (run `lambda_20260903T071915Z`, arm `timbre2_sh15`, whose
configuration is what `timbre2` does now). Re-pin only after an intentional
accepted change: copy fresh `metrics.json` values, set
`environment.manifest_sha256` from `run.json` / `manifest.json`, and recentre
the `ci` bands.

## Results

Dataset for every run below: FMA-small, 7994 analyzed / 8000 prepared, artist
filter on, k=100, manifest
`6bf64128a580fe4d54b32d9e3d410b2f028126a055b4e3a88e6d3f65ee31a3ec`.

### Current method state

`timbre2` ships covariance shrinkage λ=0.15, chosen in tuning round 2
(`eval/TUNING_RESULTS.md`). Numbers from run
`lambda_20260903T071915Z/fma_small_20260903T071915Z`:

| Method | P@1 | P@5 | P@10 | P@20 | MAP@100 | knn@5 | hub skew | hub max | AUC |
|--------|-----|-----|------|------|---------|-------|----------|---------|-----|
| timbre2 (λ=0.15, current) | 0.379 | 0.365 | 0.354 | 0.338 | 0.0154 | 0.437 | 2.48 | 82 | 0.575 |
| timbre2 (λ=0.10, previous default) | 0.391 | 0.373 | 0.360 | 0.345 | 0.0160 | 0.442 | 2.75 | 95 | 0.579 |

λ=0.15 costs 0.60 pp of P@10 (cluster-bootstrap CI [−0.76, −0.44] pp) and buys
an improvement on **both** hubness statistics; the full trade-off table for all
eight grid arms is in `eval/TUNING_RESULTS.md`.

### Genre ranking, timbre vs timbre2 (artist-filtered)

Run: `eval/results/fma_small_20260831T103326Z`

> These two rows predate the resampler clipping fix (commit `7d3c471`), so they
> are not bit-comparable with the round-2 numbers above: the fix changed the
> features of every track whose decoded signal exceeds full scale. In aggregate
> it turned out to be metric-neutral — the same `timbre2` configuration
> (λ=0.10) scores P@10 0.3604 before the fix and 0.3602 after, with hub
> skewness 2.758 → 2.746 and hub max 96 → 95 — so the comparison below still
> holds directionally, but `timbre` has not been re-measured on fixed features.

| Method | P@1 | P@5 | P@10 | P@20 | MAP@100 | knn@5 | hubness skew | AUC |
|--------|-----|-----|------|------|---------|-------|--------------|-----|
| timbre | 0.377 | 0.357 | 0.347 | 0.334 | 0.0153 | 0.425 | 2.23 | 0.581 |
| timbre2 (λ=0.10) | 0.391 | 0.373 | 0.360 | 0.345 | 0.0160 | 0.443 | 2.76 | 0.579 |

Paired `timbre` → `timbre2` on P@10 (`compare.json`):

- ΔP@10 = **+0.0132** (wins 2598 / losses 2002 / ties 3394)
- Wilcoxon p ≈ 4.2×10⁻¹⁸
- Cluster-bootstrap 95% CI: **[0.0094, 0.0167]** (entirely above 0)
- Same sign for ΔP@1 / ΔP@5 / ΔP@10 / ΔP@20 (all positive)
- Overlap@10 ≈ 0.50, Kendall τ-b@10 ≈ 0.24 (rankings changed, not identical)
- Per-genre: gains on Folk, Hip-Hop, Rock, International, Pop; small losses on Electronic and Instrumental

### Perturbation robustness (200 sources × 8 variants)

Siblings excluded (current default):

| Method | overall top-1 | vol_m12 | vol_p6 | mp3_64k | lowpass8k | rate16k | silence_pre20 | mid15s | loop4m |
|--------|---------------|---------|--------|---------|-----------|---------|---------------|--------|--------|
| timbre2 (λ=0.15, current) | 0.844 | 1.00 | 1.00 | 0.67 | 1.00 | 0.11 | 0.99 | 0.99 | 1.00 |
| timbre2 (λ=0.10, pre-fix run) | 0.845 | 1.00 | 1.00 | 0.65 | 1.00 | 0.13 | 1.00 | 0.99 | 1.00 |
| timbre (pre-fix run) | 0.853 | 1.00 | 1.00 | 0.73 | 1.00 | 0.11 | 1.00 | 0.99 | 1.00 |

**Why these numbers replaced the earlier ones.** The collection contains all
eight variants of every source track, so two near-identical variants compete
with the original and push it below rank 1. The metric was therefore largely
measuring "is the original ahead of my seven siblings". With
`--include-siblings` the same dumps give overall top-1 0.333 / 0.336 and
`vol_p6` 0.11 / 0.10 — but a sibling outranked the original for **90.5 %** of
`vol_p6` queries, so that figure says almost nothing about gain robustness.

Consequences: volume robustness is effectively perfect (`vol_m12`, `vol_p6`
both ≈ 1.00), and so are silence padding, looping, cropping and lowpass. The
genuine weaknesses are `mp3_64k` (0.65–0.73) and `rate16k` (0.11–0.13). All
methods land within ~1 pp of each other overall, so this suite does not
separate `timbre` from `timbre2`.

An independent check on gain invariance is in `eval/data/gaincheck` (see
`eval/make_gain_fixtures.py`, `eval/run_gaincheck.sh`): a lossless +6 dB WAV
variant now yields Gaussian features identical to the original to six
decimals, after the resampler clipping fix described in
`eval/TUNING_RESULTS.md`.

### Verdict

Against the written decision rules:

1–3 (ranking significance / CI / consistent P@K sign): **pass** for `timbre2`
4 (hubness): **partially addressed** — deltas raised skewness 2.23 → 2.76 and
   max k-occurrence 69 → 96; shrinkage λ=0.15 brings the shipped method back to
   2.48 / 82, still above `timbre` but roughly half the regression  
5 (perturbations): **pass** — 0.844 for the shipped method against 0.853 for
   `timbre`, the gap driven by `mp3_64k` (0.67 vs 0.73); every other variant is
   tied at ≈ 1.00

**Conclusion:** MFCC deltas improve artist-filtered genre ranking by a small
but statistically solid margin (≈ +1.3 pp P@10 at λ=0.10), and the shipped
configuration trades 0.6 pp of that back for a hubness improvement on both
statistics (≈ +0.7 pp P@10 over `timbre`, skewness 2.48 vs 2.23). That is a
deliberate trade, not a blanket quality win: hubness is still worse than
`timbre`, so watch it on large collections.

Stage-3 robustness is in better shape than the first version of this report
claimed: gain, silence padding, looping, cropping and lowpass are all at
ceiling, and gain invariance is now exact. The open robustness questions are
narrower and shared by both methods — aggressive mp3 quantization (`mp3_64k`)
and low-sample-rate input (`rate16k`, 0.11–0.13, the one clear failure).

Manual odd-one-out annotation is optional; automatic metrics are already
decisive on ranking, and the main open questions are hubness and the
`rate16k` / `mp3_64k` floors rather than ties between the two methods.

## Implementation notes tied to Musly

- `tracks_add` sorts paths and appends in index order so collection order (and
  Mutual Proximity) is reproducible under OpenMP.
- `musly -s -f NUM` applies the artist filter **while** selecting neighbors
  (not after truncating to k).
- Sparse dump lines are buffered per query index so OpenMP does not scramble
  order (needed for dump SHA-256 determinism checks).
