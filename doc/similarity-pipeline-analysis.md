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
- Several evenly spaced segments instead of a single center crop
- Ledoit–Wolf shrinkage, estimation in `double`
- Collection / `MUSLY_VERSION` bump (requires re-analysis)

### Stage 4 — `timbre2`
- Parameterized `timbre` plus MFCC-delta method (±2 frame window), priority 2

## Quality evaluation protocol

1. **Self-test in the container** — `docker build` runs `ctest` (degenerate cases + numerical regression).
2. **Decoder path** — files generated with `ffmpeg` (tone, noise, stereo, silence, antiphase, 8 kHz, corrupted) → `musly -n/-a/-p/-m` (`test/decoder_smoke.sh`).
3. **kNN ablation** — `musly -E` with an artist filter on a genre-labeled collection ([MIREX / musly.org protocol](https://www.musly.org/methods.html)).
4. **Acceptance gates**
   - Stage 0: no change in results for valid audio
   - Stage 2: changes only at `float` precision
   - Stages 3–4: accept only if kNN accuracy does not drop

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

### kNN ablation (fill in on your labeled collection)

Run (mount a collection with labels in the path, e.g. `.../genre/artist/track.mp3`):

```bash
docker run --rm -v /path/to/collection:/collection -v /tmp/meta:/metadata musly:dev \
  bash -c 'cd /metadata && musly -N -c c.musly && musly -c c.musly -a /collection -x mp3 \
           && musly -c c.musly -E -f 1 -k 5'
```

| Variant | kNN accuracy | Notes |
|---------|--------------|-------|
| `mandelellis` (baseline) | _(fill in)_ | no MP |
| `timbre` (after stages 2–3) | _(fill in)_ | RMS, silence, segments, shrinkage |
| `timbre2` (default) | _(fill in)_ | + MFCC deltas |

Acceptance for stages 3–4: `timbre` / `timbre2` accuracy ≥ historical `timbre` accuracy on the same collection.

## Deliberately omitted

The mel filterbank (36 bands) is numerically sound. Optimizations that do not
change results (`erfc` instead of the CDF approximation, buffer reuse in
`similarity_raw`, `makeCompressed` on the filters) and an empirical CDF in MP
remain candidates for later work.
