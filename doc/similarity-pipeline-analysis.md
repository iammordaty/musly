# Musly Similarity Pipeline Analysis

This document describes Musly’s audio analysis and similarity pipeline, the
defects found in it, and the rationale for the changes introduced in the 0.3
modernization.

## Processing pipeline

```
audio file
  → libav (decode to 22050 Hz, mono, float)
  → analyze_track (PCM excerpt selection)
  → powerspectrum (Hann 1024, hop 512)
  → melspectrum (36 bands, 20 Hz–Nyquist)
  → mfcc (log power + DCT-II → 25 coefficients)
  → estimate_gaussian (μ, Σ, log|Σ|)
  → jensenshannon (timbre) / symmetric_kullbackleibler (mandelellis)
  → mutualproximity (distance normalization)
  → ranking (musly_findmin / kNN)
```

The former default method was `timbre` (priority 1). After adding `timbre2`
(priority 2), the default is the MFCC-delta method. `mandelellis` remains
available as the baseline variant without Mutual Proximity.

## Defects

### Correctness bugs

| ID | Location | Description | Impact on ranking |
|----|----------|-------------|-------------------|
| C1 | `powerspectrum.cpp` ~49–53 | On digital silence, `pcm_scale = 10^4.8 / 0 = ∞`, then `0·∞ = NaN` | NaNs in features and distances; one track poisons the collection |
| C2 | `gaussianstatistics.cpp` ~159 | `jensenshannon` returns `-1` when Cholesky fails | After MP, distance ≈ 0 → degenerate pair rises to the top of the list |
| C3 | `mutualproximity.cpp` ~77–81 | No guard for `std == 0` / division by `n−1` when `n=1` | NaNs in all distances after `setmusicstyle` with a single track |
| C4 | `main.cpp` ~493, ~601 | Loops to `k` even when fewer neighbors are returned | Out-of-bounds reads (UB) on small collections / artist filter |
| C5 | `libav.cpp` ~300 | `AVSEEK_FLAG_BACKWARD \|\| AVSEEK_FLAG_ANY` (logical OR) | `ANY` never applied; seeking less reliable |
| C6 | `libav.cpp` ~475 | `size_t / int` truncates duration to whole seconds | Wrong excerpt centering when file length is unknown |
| C7 | `libav.cpp` ~108–124 | `va_list` reused without `va_copy` | UB in decoder logging |
| C8 | `gaussianstatistics.cpp` ~243 | `d/2` is integer division | SKL constant shifted by 0.5 for odd `d` |

### Numerical weaknesses

| ID | Location | Description | Impact |
|----|----------|-------------|--------|
| N1 | `gaussianstatistics.cpp` ~91–98 | Commented-out `isInvertible`, QR instead of LLT | Garbage in `covar_inverse` / `-inf` in `logdet` with no failure signal |
| N2 | `gaussianstatistics.cpp` ~78 | Fixed ridge `1e-4` independent of feature scale | Conditioning depends on log-power dynamics |
| N3 | Estimation in `float` | JS is a difference of large log|Σ| values | Cancellation of significant digits |

### Algorithmic limitations

| ID | Description | Effect |
|----|-------------|--------|
| A1 | Peak normalization to 96 dB | A click/clip rescales the track; a quiet track boosts floor noise; c0 encodes crest factor (album effect) |
| A2 | `log(1+x)` instead of a dB floor | Effective floor ~−96 dB relative to peak |
| A3 | No silence-frame rejection | Silences/applause pull the Gaussian |
| A4 | Single continuous excerpt (center) | Unrepresentative for long recordings |
| A5 | No temporal information (bag-of-frames) | Same timbre + different rhythm → indistinguishable |
| A6 | MP assumes normal distances | JS is right-skewed; an empirical CDF would be more accurate (candidate, default unchanged) |

## Changes introduced

### Stage 0 — environment
- Decoder compatible with FFmpeg 7 (`ch_layout`, `av_packet_alloc`, `const AVCodec*`, drain)
- Multi-stage `Dockerfile` (Debian 13 / FFmpeg 7.1) with `ctest` in the build stage

### Stage 2 — correctness
- Silence protection / `isfinite` in estimation
- Fallback JS distance = `FLT_MAX`; skip non-finite values in MP stats
- Floor on MP `std`; validate `n ≥ 2`
- LLT decomposition with explicit failure
- Fixes C4–C8, `std::shuffle`, pointer hygiene

### Stage 3 — feature quality
- RMS normalization, log floor at −80 dB
- Silence-frame rejection (−60 dB relative to P95)
- Analysis restricted to the centered 60% of the signal, so intros and outros
  are dropped proportionally to the track length and different edits of the
  same track stay structurally aligned
- Several evenly spaced segments within that region instead of a single
  center crop spanning the whole recording
- Ledoit–Wolf shrinkage, estimation in `double`
- Collection / `MUSLY_VERSION` bump (requires re-analysis)

### Stage 4 — `timbre2`
- Parameterized `timbre` plus MFCC-delta method (±2 frame window), priority 2

### Stage 5 — Mutual Proximity reference set
- The whole collection is used as the reference set up to 8000 tracks; beyond
  that a sample of 1000 drawn from the entire collection (`tracks_initialize`)
- Saved jukebox state records a fingerprint of the collection paths, so a
  state file whose reference set no longer matches is rebuilt instead of being
  applied to the wrong tracks

### Stage 6 — jukebox performance
- `timbre::add_tracks` parallelized with OpenMP (private distance buffer per
  thread); registration of N tracks against an N-track reference set scales
  with core count
- `musly_jukebox_fromstream_lean()` skips the Mutual Proximity reference
  models in the jukebox header; the CLI uses it whenever the collection and
  fingerprint match exactly, cutting jukebox I/O from ~N·track_size down to
  ~12 bytes per track
- `-p` is repeatable and accepts `-` for stdin, so a burst of queries loads
  the collection once
- With `-J`, `-a` incrementally updates the on-disk jukebox and `-r`/`-R`
  rebuild it, so query invocations never pay for a full `tracks_initialize`

## Mutual Proximity reference set

`musly_jukebox_setmusicstyle()` does not take part in the distance between two
tracks. It fixes, per track, the mean and standard deviation of that track’s
distances to a reference sample, which `normalize()` then turns into
`1 − p1·p2`. The reference set is a measuring stick, not a term of the
measurement, which bounds the damage a bad one can do.

`test/reference_set_experiment` quantifies that damage on a synthetic
collection of 900 tracks in three disjoint frequency bands (“genres”),
comparing the whole collection against a uniform sample and against a sample
drawn from a single genre.

| Reference set | Mean distance within g1 | Spread within g1 | Top-10 agreement with full |
|---------------|-------------------------|------------------|----------------------------|
| whole collection (900) | 0.1914 | 0.097 | — |
| uniform sample (200) | 0.2219 | 0.102 | 0.918 |
| single genre (200) | 0.0001 | 0.005 | 0.339 |

Three observations:

- **Genre structure is robust.** Even with the worst reference set, the
  cross-genre share of the top-10 stayed at 0.000 and k-occurrence stayed
  neutral (10.00 per genre). A skewed reference set does not flood playlists
  with the wrong music.
- **Within-genre resolution is not.** For a genre absent from the reference
  set, distances collapsed to ~1e-4 with a spread of 5e-3: the score
  saturates and two or three significant digits are left to order hundreds of
  tracks. This is the same mechanism that produces distances printed in
  scientific notation. Top-10 agreement with the exact answer drops to 0.339.
- **Size is not the issue, representativeness is.** 200 of 900 (22%) already
  reproduces the full reference set to within 0.918 agreement. Raising the
  sample size barely helps, because the sampling error of the mean at n=1000
  is already ~3% of the standard deviation.

The caveat is that the synthetic genres are separated far more cleanly than
real ones, so cross-genre robustness would be weaker on an actual collection,
and the single-genre variant is a worst case rather than a realistic skew.

Note that with a saved jukebox state the reference set is the collection as of
the last full initialization, not necessarily the current one: tracks appended
through the incremental update path are calibrated against the stored
reference set. The existing rule that rebuilds the jukebox once the collection
has grown by more than 10% keeps that gap bounded.

Registration costs `tracks × reference` distance evaluations. Measured on the
container (`test/jukebox_baseline`, timbre2):

| N | threads | add_tracks | µs / pair | one query |
|---|---------|------------|-----------|-----------|
| 800 | 1 | 4.66 s | 7.3 | 6 ms |
| 800 | 4 | 1.68 s | 2.6 | 8 ms |
| 1500 | (default) | 4.71 s | 2.1 | 15 ms |

So wall-clock time for a full rebuild against the whole collection scales with
core count. Extrapolating to 5000 tracks: roughly half a minute on four cores
instead of the previous ~3.5 minutes single-threaded. The reference tracks are
still copied into the jukebox state file (~5.3 kB per track), but query-time
loads use `musly_jukebox_fromstream_lean()` and skip that payload when the
collection has not changed.

## Quality evaluation protocol

1. **Self-test in the container** — `docker build` runs `ctest` (degenerate cases, excerpt focus, synthetic family retrieval).
2. **Decoder path** — files generated with `ffmpeg` (tone, noise, stereo, silence, antiphase, 8 kHz, corrupted) → `musly -n/-a/-p/-m` (`test/decoder_smoke.sh`).
3. **FMA-small harness** — reproducible genre ranking with artist filter, paired stats, and perturbation robustness: see [`eval/REPORT.md`](../eval/REPORT.md) and the scripts under [`eval/`](../eval/).
4. **kNN ablation (legacy CLI)** — `musly -E` with an artist filter on a genre-labeled collection ([MIREX / musly.org protocol](https://www.musly.org/methods.html)).
5. **Reference set sensitivity** — `test/reference_set_experiment` when changing the reference set policy or the MP normalization.
6. **Acceptance gates**
   - Stage 0: no change in results for valid audio
   - Stage 2: changes only at `float` precision
   - Stages 3–4: accept only if the `eval/` decision rules say so (not merely “different neighbors”)

### Verification results (container `musly:dev`)

| Check | Result |
|-------|--------|
| `ctest` (selftest, all methods including `timbre2`) | PASSED |
| Silence / too-short PCM rejected | PASSED |
| `setmusicstyle(1)` rejected for MP methods | PASSED |
| Deterministic similarity (finite, ≥0, self=0) | PASSED |
| Decoder: tone/noise/stereo/8 kHz analyzed | PASSED |
| Decoder: silence / antiphase / corrupted rejected | PASSED |
| `musly -i` → `mandelellis,timbre,timbre2` | PASSED |

### kNN / ranking evaluation (FMA-small)

Prefer the harness over a one-off `-E` run:

```bash
python3 eval/fetch_fma.py
python3 eval/prepare_dataset.py
docker build -t musly:dev .
CHECK_DETERMINISM=1 eval/run_benchmark.sh
```

Results on FMA-small (7994 tracks, artist filter):

| Variant | P@10 | knn@5 | hub skew | Notes |
|---------|------|-------|----------|-------|
| `timbre` | 0.347 | 0.425 | 2.23 | shared stage 2–3 pipeline; run `fma_small_20260831T103326Z` |
| `timbre2` (default) | 0.354 | 0.437 | 2.48 | + MFCC deltas, shrinkage λ=0.15; run `lambda_20260903T071915Z` |

Deltas are worth ≈ +1.3 pp P@10 over `timbre` at the original shrinkage
(λ=0.10) but raise hubness; the shipped λ=0.15 gives back 0.6 pp of that to
recover part of the hubness regression. See [`eval/REPORT.md`](../eval/REPORT.md)
for the decision-rule verdict and [`eval/TUNING_RESULTS.md`](../eval/TUNING_RESULTS.md)
for the tuning grid. A changed neighbor list alone is not acceptance.

## Deliberately omitted

The mel filterbank (36 bands) is numerically sound. Optimizations that do not
change results (`erfc` instead of the CDF approximation, buffer reuse in
`similarity_raw`, `makeCompressed` on the filters) and an empirical CDF in MP
remain candidates for later work.
