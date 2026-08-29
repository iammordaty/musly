/**
 * Copyright 2026, Musly maintainers
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
 * Diagnostic tool measuring how much the Mutual Proximity reference set
 * (musly_jukebox_setmusicstyle) influences similarity results.
 *
 * It builds a synthetic collection of clearly separated "genres" and compares
 * three reference sets: the whole collection, a sample drawn uniformly from
 * it, and a sample drawn from a single genre. The single-genre variant is the
 * worst case a badly chosen reference set can produce.
 *
 * This is not a pass/fail test and is therefore not registered with ctest.
 * Run it manually when changing the reference set policy in
 * musly/main.cpp (tracks_initialize) or the normalization in
 * libmusly/mutualproximity.cpp.
 *
 * Usage: reference_set_experiment [tracks_per_genre] [reference_size]
 */

#include <musly/musly.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <numeric>
#include <random>
#include <string>
#include <vector>

static const int SAMPLE_RATE = 22050;
static const int TRACK_SAMPLES = SAMPLE_RATE * 10;
static const int NUM_GENRES = 3;
static const int TOPK = 10;

static int per_genre = 300;
static int reference_size = 200;
static int num_tracks = NUM_GENRES * 300;

static int genre_of(int track) {
    return track / per_genre;
}

/** Synthesizes a track whose genre is a property of the audio: each genre
 * occupies its own frequency band.
 */
static void generate_genre(float* out, int length, unsigned int seed, int genre) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float band_lo[NUM_GENRES] = {60.0f, 500.0f, 2000.0f};
    const float band_hi[NUM_GENRES] = {300.0f, 1200.0f, 5000.0f};

    std::fill(out, out + length, 0.0f);
    const int partials = 6 + (int)(u(rng) * 12);
    for (int i = 0; i < partials; i++) {
        const int len = length / 10 + (int)((length / 10) * u(rng));
        const int start = (int)((length - len) * u(rng));
        const float basefreq =
                band_lo[genre] + (band_hi[genre] - band_lo[genre]) * u(rng);
        const float baseamp = 0.1f + 0.9f * u(rng);
        const float tremolosize = std::abs(baseamp - 0.5f) * u(rng);
        const float tremolospeed = 5.0f * std::pow(u(rng), 3.0f);
        for (int s = start; s < start + len; s++) {
            const float t = 2.0f * (float)M_PI * s / (float)SAMPLE_RATE;
            const float amp = baseamp + tremolosize * std::sin(t * tremolospeed);
            out[s] += amp * std::sin(t * basefreq);
        }
    }

    float absmax = 0.0f;
    for (int s = 0; s < length; s++) {
        absmax = std::max(absmax, std::abs(out[s]));
    }
    if (absmax > 0.0f) {
        for (int s = 0; s < length; s++) {
            out[s] /= absmax;
        }
    }
}

struct result {
    std::string name;
    double genre_dist[NUM_GENRES][NUM_GENRES];
    double within_std[NUM_GENRES];
    double kocc[NUM_GENRES];
    double cross[NUM_GENRES];
    std::vector<std::vector<int> > top;
    double register_seconds;
    long long register_pairs;
};

static result evaluate(
        const char* name,
        std::vector<musly_track*>& tracks,
        const std::vector<int>& reference) {
    musly_jukebox* jukebox = musly_jukebox_poweron("timbre2", NULL);
    if (!jukebox) {
        std::fprintf(stderr, "could not power on the jukebox\n");
        std::exit(1);
    }

    std::vector<musly_track*> reftracks;
    for (int i = 0; i < (int)reference.size(); i++) {
        reftracks.push_back(tracks[reference[i]]);
    }

    std::vector<musly_trackid> trackids(num_tracks, -1);
    const std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
    if (musly_jukebox_setmusicstyle(jukebox, reftracks.data(),
            (int)reftracks.size()) != 0) {
        std::fprintf(stderr, "setmusicstyle failed\n");
        std::exit(1);
    }
    if (musly_jukebox_addtracks(jukebox, tracks.data(), trackids.data(),
            num_tracks, 1) != 0) {
        std::fprintf(stderr, "addtracks failed\n");
        std::exit(1);
    }
    const std::chrono::steady_clock::time_point registered =
            std::chrono::steady_clock::now();

    result r;
    r.name = name;
    r.register_seconds =
            std::chrono::duration<double>(registered - start).count();
    r.register_pairs = (long long)num_tracks * (long long)reftracks.size();
    std::memset(r.genre_dist, 0, sizeof(r.genre_dist));
    r.top.assign(num_tracks, std::vector<int>());

    double within_sq[NUM_GENRES] = {0.0, 0.0, 0.0};
    double cross_sum[NUM_GENRES] = {0.0, 0.0, 0.0};
    long long pairs[NUM_GENRES][NUM_GENRES];
    std::memset(pairs, 0, sizeof(pairs));
    std::vector<int> kocc(num_tracks, 0);

    std::vector<float> sim(num_tracks);
    std::vector<int> order;
    for (int seed = 0; seed < num_tracks; seed++) {
        if (musly_jukebox_similarity(jukebox, tracks[seed], trackids[seed],
                tracks.data(), trackids.data(), num_tracks, sim.data()) != 0) {
            std::fprintf(stderr, "similarity failed\n");
            std::exit(1);
        }

        for (int other = 0; other < num_tracks; other++) {
            if (other == seed) {
                continue;
            }
            const int a = genre_of(seed);
            const int b = genre_of(other);
            r.genre_dist[a][b] += sim[other];
            pairs[a][b]++;
            if (a == b) {
                within_sq[a] += (double)sim[other] * (double)sim[other];
            }
        }

        // The candidate list has to be rebuilt for every seed, not reused.
        order.resize(num_tracks);
        std::iota(order.begin(), order.end(), 0);
        order.erase(order.begin() + seed);
        std::partial_sort(order.begin(), order.begin() + TOPK, order.end(),
                [&sim](int a, int b) { return sim[a] < sim[b]; });
        r.top[seed].assign(order.begin(), order.begin() + TOPK);
        for (int k = 0; k < TOPK; k++) {
            kocc[order[k]]++;
            if (genre_of(order[k]) != genre_of(seed)) {
                cross_sum[genre_of(seed)] += 1.0;
            }
        }
    }

    for (int a = 0; a < NUM_GENRES; a++) {
        for (int b = 0; b < NUM_GENRES; b++) {
            r.genre_dist[a][b] /= (double)pairs[a][b];
        }
        const double mean = r.genre_dist[a][a];
        const double var =
                within_sq[a] / (double)pairs[a][a] - mean * mean;
        r.within_std[a] = std::sqrt(std::max(var, 0.0));

        double occurrences = 0.0;
        for (int i = a * per_genre; i < (a + 1) * per_genre; i++) {
            occurrences += kocc[i];
        }
        r.kocc[a] = occurrences / (double)per_genre;
        r.cross[a] = cross_sum[a] / (double)(per_genre * TOPK);
    }

    musly_jukebox_poweroff(jukebox);
    return r;
}

/** Average share of the top-10 list that two variants agree on. */
static double overlap(const result& a, const result& b) {
    double sum = 0.0;
    for (int seed = 0; seed < num_tracks; seed++) {
        std::vector<int> x = a.top[seed];
        std::vector<int> y = b.top[seed];
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        std::vector<int> shared;
        std::set_intersection(x.begin(), x.end(), y.begin(), y.end(),
                std::back_inserter(shared));
        sum += shared.size() / (double)TOPK;
    }
    return sum / (double)num_tracks;
}

static void report(const result& r) {
    std::printf("\n--- reference set: %s ---\n", r.name.c_str());
    std::printf("mean MP distance (rows = seed genre, cols = candidate genre)\n");
    for (int a = 0; a < NUM_GENRES; a++) {
        std::printf("  g%d:", a);
        for (int b = 0; b < NUM_GENRES; b++) {
            std::printf("  %.4f", r.genre_dist[a][b]);
        }
        std::printf("\n");
    }
    std::printf("spread of same-genre distances (std dev):     ");
    for (int a = 0; a < NUM_GENRES; a++) {
        std::printf(" g%d=%.5f", a, r.within_std[a]);
    }
    std::printf("\nmean k-occurrence per track (%.2f = neutral):", (double)TOPK);
    for (int a = 0; a < NUM_GENRES; a++) {
        std::printf(" g%d=%.2f", a, r.kocc[a]);
    }
    std::printf("\ncross-genre share of top-%d:                  ", TOPK);
    for (int a = 0; a < NUM_GENRES; a++) {
        std::printf(" g%d=%.3f", a, r.cross[a]);
    }
    std::printf("\nregistration: %.2f s for %lld distance evaluations "
            "(%.1f us each)\n", r.register_seconds, r.register_pairs,
            1e6 * r.register_seconds / (double)r.register_pairs);
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        per_genre = std::atoi(argv[1]);
    }
    if (argc > 2) {
        reference_size = std::atoi(argv[2]);
    }
    num_tracks = NUM_GENRES * per_genre;
    if ((per_genre < 2) || (reference_size < 2)
            || (reference_size > num_tracks) || (per_genre < reference_size)) {
        std::fprintf(stderr, "usage: %s [tracks_per_genre] [reference_size]\n"
                "  reference_size must be at least 2 and at most "
                "tracks_per_genre\n", argv[0]);
        return 1;
    }

    std::printf("Generating and analyzing %d tracks in %d genres...\n",
            num_tracks, NUM_GENRES);
    musly_jukebox* setup = musly_jukebox_poweron("timbre2", NULL);
    if (!setup) {
        std::fprintf(stderr, "could not power on the jukebox\n");
        return 1;
    }

    std::vector<musly_track*> tracks(num_tracks, NULL);
    std::vector<float> pcm(TRACK_SAMPLES);
    for (int i = 0; i < num_tracks; i++) {
        generate_genre(pcm.data(), TRACK_SAMPLES, 1000 + i * 7, genre_of(i));
        tracks[i] = musly_track_alloc(setup);
        if (musly_track_analyze_pcm(setup, pcm.data(), TRACK_SAMPLES,
                tracks[i]) != 0) {
            std::fprintf(stderr, "analysis failed for track %d\n", i);
            return 1;
        }
    }
    std::printf("track size: %d bytes\n", musly_track_size(setup));
    musly_jukebox_poweroff(setup);

    std::vector<int> everything(num_tracks);
    std::iota(everything.begin(), everything.end(), 0);

    std::vector<int> balanced(everything);
    std::mt19937 rng(42);
    std::shuffle(balanced.begin(), balanced.end(), rng);
    balanced.resize(reference_size);

    std::vector<int> biased(reference_size);
    std::iota(biased.begin(), biased.end(), 0);

    const result full = evaluate("whole collection", tracks, everything);
    const result sampled = evaluate("uniform sample", tracks, balanced);
    const result skewed = evaluate("sample from one genre", tracks, biased);

    report(full);
    report(sampled);
    report(skewed);

    std::printf("\n--- top-%d agreement with the whole collection ---\n", TOPK);
    std::printf("uniform sample:        %.3f\n", overlap(full, sampled));
    std::printf("sample from one genre: %.3f\n", overlap(full, skewed));

    for (int i = 0; i < num_tracks; i++) {
        musly_track_free(tracks[i]);
    }
    return 0;
}
