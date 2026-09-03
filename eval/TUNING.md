# Low-risk tuning experiments (timbre2 baseline)

> Historical document: this is the round-1 experiment catalogue, written when
> the baseline was `timbre2` with shrinkage λ=0.1 and no variant was merged.
> It does not describe the current method set — λ=0.15 is now part of `timbre2`
> and the CSLS path is gone. Outcomes are in `TUNING_RESULTS.md`.

Baseline run: `fma_small_20260831T103326Z` — P@10 **0.360**, hubness skew **2.76**,
perturbation vol_m12 **0.60**, vol_p6 **0.10** (timbre2).

Parameters located in code today:

| Parameter | Location | Current |
|-----------|----------|---------|
| `target_rms` | `libmusly/powerspectrum.cpp:68` | 0.1 (−20 dBFS) |
| `log_floor` | `libmusly/mfcc.cpp:30` | 1e-8 |
| `focus_fraction` | `libmusly/methods/timbre.cpp:53` | 0.6 |
| silence gate | `timbre::select_frames` | P95 − 60 dB |
| `delta_width` | `timbre.cpp:50` | 2 |
| delta scale | (none) | 1.0 |
| shrinkage λ | `gaussianstatistics.cpp:82` | 0.1 |
| MP normalize | `mutualproximity.cpp:217` | `1 − p1·p2` on raw JSD |
| CSLS / hub correction | — | none |
| MP reference | `main.cpp tracks_initialize` | full coll. if ≤8000 |

## Ranked experiments (TOP 10)

### 1. Pre-MP CSLS on raw JSD — **implemented as `timbre2_cs`**
- **Change:** `sim ← max(0, 2·d − μ_seed − μ_other)` before MP; μ already stored in `norm_facts`.
- **Where:** `timbre::similarity()` (`timbre.cpp`).
- **Targets:** hubness (primary), possibly small P@10 trade-off.
- **Grid:** on/off (A/B).
- **Expected:** skew 2.76 → ~2.0–2.4; ΔP@10 −0.3…−1.0 pp possible.
- **Risk:** low (query-time only, no re-analysis). **Cost:** O(n) like MP.
- **Side effects:** may flatten fine genre structure if μ estimates noisy.

### 2. Delta scale 0.5 for timbre2 — **implemented as `timbre2_d05`**
- **Change:** stack `0.5 × deltas` with full MFCCs before Gaussian fit.
- **Where:** `timbre::analyze_track()` delta branch.
- **Targets:** hubness (deltas add 25 dims → hub vectors), keep most of ΔP@10.
- **Grid:** {0.35, 0.5, 0.7, 1.0}.
- **Expected:** skew down; P@10 between timbre and timbre2.
- **Risk:** medium (feature change, re-analyze). **Cost:** none at query time.

### 3. Covariance shrinkage λ=0.15 — **implemented as `timbre2_sh15`**
- **Change:** Ledoit-Wolf-style λ in `estimate_gaussian`.
- **Where:** `gaussianstatistics.cpp`.
- **Targets:** perturbations (smoother Gaussians), mild hubness.
- **Grid:** {0.10, 0.15, 0.20, 0.25}.
- **Expected:** vol_* +2–5 pp top-1; P@10 ±0.5 pp.
- **Risk:** low. **Cost:** none.

### 4. Robust MP stats (median + MAD)
- **Change:** replace mean/std in `set_normfacts` with median/MAD.
- **Targets:** hubness.
- **Risk:** medium (changes all MP distances). Not implemented (orthogonal to #1).

### 5. Lower `target_rms` (0.05) or peak-ceiling before RMS
- **Change:** `powerspectrum.cpp` — cap peak before scaling.
- **Targets:** vol_p6 (clip from ffmpeg +6 dB), vol_* overall.
- **Grid:** target_rms {0.05, 0.1, 0.15}, peak cap {0.95, 0.99}.
- **Note:** vol_p6 failure may be **clipping in ffmpeg**, not Musly RMS — verify with `ffmpeg -af volumedetect`.

### 6. Widen silence gate (−50 dB vs −60 dB)
- **Change:** `threshold = p95 * 1e-5f`.
- **Targets:** silence_pre20 perturbation.
- **Risk:** low. Grid: {1e-5, 1e-6, 1e-7}.

### 7. `delta_width` {1, 2, 3}
- **Targets:** P@10 vs hubness trade-off for timbre2.
- **Risk:** low-medium.

### 8. `focus_fraction` {0.5, 0.6, 0.7}
- **Targets:** mid15s perturbation, genre ranking on 30 s clips.
- **Risk:** medium on P@10.

### 9. Post-MP query z-score (local rank calibration)
- **Change:** subtract median of top-100 raw MP distances per query.
- **Targets:** hubness without CSLS.
- **Risk:** can distort MP semantics; test after CSLS.

### 10. Reduce MP reference sample (8000 → 2000 stratified)
- **Change:** `tracks_initialize` cap — **not recommended** (already fixed at 8000 full set).
- **Targets:** speed only; likely hurts stability.

## Implemented variants

| Method | Experiment | Re-analyze? |
|--------|------------|-------------|
| `timbre2` | baseline | — |
| `timbre2_cs` | CSLS pre-MP | no (same features) |
| `timbre2_d05` | delta scale 0.5 | yes |
| `timbre2_sh15` | shrinkage λ=0.15 | yes |

Run: `./eval/run_tuning.sh` (rebuilds Docker, evaluates all variants vs baseline).
