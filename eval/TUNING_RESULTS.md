# Tuning results (timbre2 variants vs pinned baseline)

Baseline run: `eval/results/fma_small_20260831T103326Z` (FMA-small, 7994 tracks,
k=100, artist-filtered). Perturbations: 200 originals × 8 variants.
Experiment catalogue and rationale: `eval/TUNING.md`.

## Reference numbers

| Method | P@10 | knn@5 | AUC | hub skew | hub max | pert top-1 |
|--------|------|-------|-----|----------|---------|------------|
| `timbre` | 0.3472 | 0.4246 | 0.5813 | 2.226 | 69 | 0.3331 |
| `timbre2` (baseline) | 0.3604 | 0.4431 | 0.5786 | 2.758 | 96 | 0.3356 |

## Evaluated variants

| Method | P@10 | ΔP@10 | knn@5 | AUC | hub skew | hub max | pert top-1 | vol_m12 | vol_p6 |
|--------|------|-------|-------|-----|----------|---------|------------|---------|--------|
| `timbre2` | 0.3604 | — | 0.4431 | 0.5786 | 2.758 | 96 | 0.3356 | 0.600 | 0.095 |
| `timbre2_cs` (CSLS, fixed) | **0.3639** | **+0.35 pp** | 0.4483 | 0.5846 | 2.886 | **70** | **0.3550** | 0.605 | 0.105 |
| `timbre2_d05` (delta ×0.5) | 0.3541 | −0.63 pp | 0.4348 | 0.5722 | 2.737 | 95 | 0.3244 | 0.590 | 0.090 |
| `timbre2_sh15` (λ=0.15) | 0.3542 | −0.62 pp | 0.4366 | 0.5745 | **2.491** | 83 | 0.3263 | 0.585 | 0.090 |

Run dirs: `timbre2_cs` → `eval/results/tuning_20260901T141015Z`,
`timbre2_d05` → `eval/results/tuning_20260901T112659Z/fma_small_20260901T120708Z`,
`timbre2_sh15` → `eval/results/tuning_20260901T145342Z/fma_small_20260901T145342Z`.

## Experiment 1 — CSLS hub penalty (`timbre2_cs`)

### First implementation was wrong (recorded, not a result about CSLS)

The initial version transformed the raw JSD before `mp.normalize()`
(`sim = max(0, 2·d − μ_seed − μ_other)`) but left MP comparing that transformed
value against the **untransformed** `μ`/`σ` stored in `norm_facts`. Two failures
compounded:

1. `max(0, ·)` clamps roughly every pair closer than average to exactly `0`.
2. `0` sits far below `μ`, so `p1 ≈ p2 ≈ 1` and MP returns `1 − p1·p2 ≈ 0`
   for all of them — thousands of exact ties per query.

Measured: P@10 **0.1489** (≈ genre prior), hub skew **28.4**, hub max **2046**,
perturbation top-1 **0.0006**. This is a broken pipeline, not evidence against CSLS.

### Fixed implementation

The correction now lives in `mutualproximity::normalize_csls()`
(`libmusly/mutualproximity.cpp`) so MP's reference distributions are shifted
consistently with the transformed distance. For `dc = 2d − μ_s − μ_o`, seen from
track `t` the reference mean becomes `μ_t − μ̄` and the deviation `2σ_t`:

```
p1 = 1 − Φ((dc − (μ_s − μ̄)) / 2σ_s)
p2 = 1 − Φ((dc − (μ_o − μ̄)) / 2σ_o)
```

Expanding `p1` gives `(d − μ_s)/σ_s + (μ̄ − μ_o)/2σ_s`: plain MP plus a penalty
on tracks whose mean distance to the collection is below average — i.e. hubs.
No clamping, no ties. `timbre.cpp` just picks the normalizer.

### Result: mild win on every metric except hub skewness

- **P@10 0.3639 (+0.35 pp over timbre2, +1.67 pp over timbre)** — the ranking
  gain from deltas is kept and slightly extended.
- **Hub max 96 → 70**, i.e. back to the `timbre` level (69). The dominance of the
  single most popular embedding is fully corrected.
- **Hub skewness 2.758 → 2.886 (worse).** CSLS cuts the extreme tail but the
  bulk of the hub distribution tightens, so the skewness statistic moves the
  other way. The two hubness metrics genuinely disagree here — reported as a
  trade-off, not hidden.
- **Perturbations 0.3356 → 0.3550 (+1.9 pp)**, driven by `loop4m` 0.42 → 0.485,
  `mid15s` 0.50 → 0.55, `silence_pre20` 0.14 → 0.155.
- `vol_m12` 0.600 → 0.605 and `vol_p6` 0.095 → 0.105 — inside noise for n=200.
  **Volume robustness is not fixed by this change.**
- AUC 0.5786 → 0.5846, knn@5 0.4431 → 0.4483.

Cost: query-time only, no re-analysis, one extra O(n) pass over `norm_facts`.

## Experiment 2 — delta scale 0.5 (`timbre2_d05`)

Clear failure on all three targets: P@10 −0.63 pp, hub skew 2.758 → 2.737
(−0.02, noise), hub max 96 → 95, perturbations 0.3356 → 0.3244 (−1.1 pp),
`vol_m12` 0.600 → 0.590. Down-weighting deltas gives back part of the ranking
gain without buying hubness, so the hubness regression does **not** come from
the delta dimensions. Revert.

## Experiment 3 — covariance shrinkage λ=0.15 (`timbre2_sh15`)

The stated hypothesis was **wrong**: smoother Gaussians did not help perturbation
robustness (0.3356 → 0.3263, `vol_m12` 0.600 → 0.585, `vol_p6` 0.095 → 0.090).

It is, however, the only variant that moves hub **skewness**: 2.758 → 2.491
(−0.267, roughly half the regression against `timbre`'s 2.226), with hub max
96 → 83. The price is P@10 −0.62 pp, knn@5 −0.65 pp and AUC −0.41 pp.

Read together with experiment 2 this locates the hubness regression: scaling the
delta dimensions down (`d05`) does nothing to skewness (−0.02), while widening
the covariance shrinkage does. **The skewness regression comes from the sharpness
of the covariance estimate in the higher-dimensional (50-d) feature space, not
from the delta dimensions themselves.**

Not worth shipping as-is — it trades ranking for one hubness statistic — but it
is the right knob for a follow-up grid.

## Experiment A — is the weak `vol_p6` result a fixture problem?

Motivated by every variant moving `vol_p6` by ≤1 pp. Three findings, in the
order they were established; the first one was wrong and is kept on the record
because it explains the second.

### A1. The `volumedetect` verdict was wrong

`eval/check_clipping.py` reported 95 % of `vol_p6` fixtures "clipped" with a
median 5.9 dB of headroom lost. **That conclusion does not hold.** ffmpeg's
`volumedetect` caps `max_volume` at 0.0 dB and lumps every sample at or above
0 dBFS into `histogram_0db`, so it cannot distinguish hard clipping from
above-full-scale float content. Measured with `astats` instead, a `vol_p6`
fixture has a true peak of **+7.78 dB** and max level 2.37 — mp3 decodes to
float and carries the boost intact. The fixtures are fine.

### A2. Musly clipped the signal itself, in the resampler

`resampler::resample()` hard-limited every output sample to [-1, 1]. FMA is
44.1 kHz and Musly targets 22.05 kHz, so **every** track passes through it. The
RMS normalization in `powerspectrum::from_pcm()` is scale invariant as written,
but it runs *after* that clamp, so it could not undo the distortion. The
originals were affected too: the measured original peaked at +1.88 dB.

Fixed by attenuating uniformly by `1/peak` when the peak exceeds full scale,
which keeps the [-1, 1] output contract without distorting the waveform
(~12 lines in `libmusly/resampler.cpp`).

Verified directly with `musly -d`: after the fix the Gaussian features of
`originals/X.mp3`, `f32_p0/X.wav` (lossless copy) and `f32_p6/X.wav` (same
audio +6 dB) agree to six decimals — `covar_logdet` -17.389666 / -17.389666 /
-17.389668. Musly is now exactly gain invariant. Before the fix the same
comparison differed substantially.

### A3. The perturbation metric was confounded by sibling variants

The suite puts 200 originals and **eight variants of each** into one collection,
then asks whether the original is the top neighbor of a variant. Once two
variants of the same track are near-identical, they compete with the original,
so the metric largely measured "is the original ahead of my seven siblings".

Sibling exclusion is now the default in `eval/score_perturbations.py`
(`--include-siblings` reproduces the old numbers). Re-scoring the existing
dumps is pure post-processing — seconds, same features. `timbre2`:

| Variant | Confounded top-1 | Corrected top-1 | Sibling ranked ahead |
|---|---|---|---|
| `vol_p6` | 0.095 | **0.995** | 90.5 % |
| `vol_m12` | 0.600 | **1.000** | 40.0 % |
| `silence_pre20` | 0.140 | **0.995** | 86.0 % |
| `loop4m` | 0.420 | **1.000** | 58.0 % |
| `mid15s` | 0.500 | **0.990** | 50.0 % |
| `lowpass8k` | 0.825 | **1.000** | 17.5 % |
| `mp3_64k` | 0.105 | **0.650** | 60.0 % |
| `rate16k` | 0.000 | **0.130** | 15.0 % |

Overall corrected top-1: `timbre` 0.853, `timbre2` 0.845, `timbre2_cs` 0.841,
`timbre2_d05` 0.845, `timbre2_sh15` 0.846 — against 0.333/0.336/0.355/0.324/0.326
on the confounded metric.

**So loudness robustness was never a real problem.** The premise "vol_* are far
from 1.0" was an artifact of the protocol. What survives as a genuine weakness
is `mp3_64k` (0.65) and above all `rate16k` (0.13), and note that all methods
sit within 1.2 pp of each other on the corrected metric — the perturbation suite
does not separate them, which the confounded numbers made look otherwise
(`timbre2_cs` "+1.9 pp" is reversed to −0.4 pp once corrected).

### Status of the resampler fix: kept, unvalidated

The fix is correct and removes a real bug, but experiment A also shows it was
not what held `vol_p6` back — on the corrected metric `vol_p6` was already at
0.995 before it.

It was **accepted without a ranking run** as an obvious bug fix. Two open
consequences follow from that decision:

- Its effect on P@10 and hubness is unmeasured. It changes the features of
  every track whose decoded signal exceeds full scale, which on FMA-small is
  most of them, so the effect is not necessarily small.
- `eval/baselines/fma_small.json` no longer reproduces (`dump_sha256`, and the
  metric bands are unverified). Re-pin from the next full run.

## Runtime behaviour that looks like a hang (it is not)

`tracks_add` (`musly/main.cpp`) analyzes a whole directory first and only then
prints `Analyzing [i]: … - [OK]` while appending in sorted index order. With
`-a` issued per genre directory (1000 tracks each) and `OMP_NUM_THREADS=1`,
that means **~17 minutes of complete silence per genre**, ~2.2 h for the
collection, before any progress line appears. Nothing is stuck.

Two side findings from investigating it:

- **Parallel `-a` is not reproducible.** Two runs with `OMP_NUM_THREADS=8` over
  the same 60 tracks produced different collection files
  (`da38c5eb…` vs `6fede000…`), while two single-threaded runs were byte
  identical (`a4c1d189…` twice). The analysis loop writes only to its own
  `serialized[i]`/`status[i]`, but every thread calls into the **shared** method
  instance (`ps`, `mel`, `mfccs`, `gs` in `timbre`) and the shared decoder via
  `mj`, so their scratch buffers race. `eval/run_benchmark.sh` already pins
  `OMP_NUM_THREADS=1`, so every number in this document is unaffected — but the
  CLI's parallel add path should not be used for anything reproducible.
- **Killed runs leave containers behind.** Three `musly:dev` containers from the
  aborted run kept burning 17–21 % CPU each for 28 h. Kill them with
  `docker ps -q --filter ancestor=musly:dev | xargs docker kill` after
  interrupting a benchmark.

## Harness fixes made during this round

- `eval/clone_collection.py` + `eval/dump_metrics_only.sh`: for query-time-only
  variants, rewrite the collection header method name and reuse the baseline
  features. Saves ~1.5 h of re-analysis per variant.
- `eval/run_benchmark.sh`, `eval/dump_metrics_only.sh`: the AUC subsample was
  added with one `musly -a` call per track, i.e. 2000 Docker containers
  (≈35 min, looked like a hang). `-a` may be repeated, so it is now a single
  invocation.
- `eval/summarize_tuning.py`: also reports `hub max`, `knn@5` and AUC, because
  hub skewness alone hid the 96 → 70 improvement; and it now walks nested run
  directories, since `run_benchmark.sh` writes `<run>/fma_small_<stamp>/<method>`
  while the clone path writes `<run>/<method>`.

## Recommendation

**Keep:** `mutualproximity::normalize_csls()` and the `csls_pre_mp` flag. It is
the only change that improves ranking, perturbations and hub dominance at once,
it costs one O(n) pass at query time, and it needs no re-analysis. It stays
opt-in via `timbre2_cs`; promoting it into `timbre2` is a separate decision,
because hub skewness moves the wrong way and that is the metric the baseline pin
tracks.

**Reverted (done).** `timbre2_d05` and `timbre2_sh15` were removed as registered
methods after evaluation — headers deleted, registrations dropped from
`libmusly/lib.cpp` and `libmusly/methods/timbre2_tuning.cpp`. Their parameters
stay in the code at inert defaults: `timbre::delta_scale` (1.0) and
`gaussian_statistics::shrinkage_lambda` (0.1). λ=0.15 remains the only lever
that moved hub skewness, so it is the starting point for the follow-up grid, but
at −0.62 pp P@10 it is not worth shipping on its own. After the revert
`musly -i` lists `mandelellis,timbre,timbre2,timbre2_cs`.

**Next smallest tuning step**, in order:

1. **λ grid on the shrinkage** — `{0.20, 0.25}` on top of `timbre2_cs`. The two
   effects are independent (λ is analysis-time, CSLS is query-time), so the
   combination could keep CSLS's P@10 and perturbation gains while taking sh15's
   skewness improvement. This is the only open path to fixing hubness on both
   statistics. Cost: ~4 h per λ (full re-analysis).
2. **Check `vol_p6` for ffmpeg clipping before touching Musly** —
   `ffmpeg -af volumedetect` on the `+6 dB` variants. Every variant tried moved
   `vol_p6` by ≤1 pp (0.090–0.105), which is what you would expect if the
   variant files are already clipped and the information is gone before Musly
   sees them. Minutes of work, and it decides whether `target_rms` tuning
   (`libmusly/powerspectrum.cpp:68`) is worth anything at all.
3. **Do not** treat `rate16k` as a tuning target. It is 0.0 top-1 for every
   method including the fixed CSLS; that is a resampling/feature-extraction
   issue, not a scoring one.
