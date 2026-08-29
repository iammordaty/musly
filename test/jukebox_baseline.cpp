/**
 * Copyright 2026, Musly maintainers
 *
 * Baseline timing for jukebox build / load / query costs.
 * Not registered with ctest. Run manually:
 *   ./jukebox_baseline [num_tracks]
 */

#include <musly/musly.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

static const int SAMPLE_RATE = 22050;
static const int TRACK_SAMPLES = SAMPLE_RATE * 5;

static void generate_track(float* out, int length, unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::fill(out, out + length, 0.0f);
    const int partials = 4 + (int)(u(rng) * 8);
    for (int i = 0; i < partials; i++) {
        const int len = length / 8 + (int)((length / 8) * u(rng));
        const int start = (int)((length - len) * u(rng));
        const float basefreq = 80.0f + 4000.0f * u(rng);
        const float baseamp = 0.2f + 0.8f * u(rng);
        for (int s = start; s < start + len; s++) {
            const float t = 2.0f * (float)M_PI * s / (float)SAMPLE_RATE;
            out[s] += baseamp * std::sin(t * basefreq);
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

static double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char* argv[]) {
    int num_tracks = 1500;
    if (argc > 1) {
        num_tracks = std::atoi(argv[1]);
    }
    if (num_tracks < 10) {
        std::fprintf(stderr, "usage: %s [num_tracks]\n", argv[0]);
        return 1;
    }

    std::printf("Baseline with %d tracks (timbre2)\n", num_tracks);
    musly_jukebox* setup = musly_jukebox_poweron("timbre2", NULL);
    if (!setup) {
        return 1;
    }

    std::vector<musly_track*> tracks(num_tracks, NULL);
    std::vector<float> pcm(TRACK_SAMPLES);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < num_tracks; i++) {
        generate_track(pcm.data(), TRACK_SAMPLES, 1000 + i * 13);
        tracks[i] = musly_track_alloc(setup);
        if (musly_track_analyze_pcm(setup, pcm.data(), TRACK_SAMPLES,
                tracks[i]) != 0) {
            std::fprintf(stderr, "analyze failed at %d\n", i);
            return 1;
        }
    }
    std::printf("analyze_pcm:          %.3f s (%.1f ms/track)\n",
            seconds_since(t0), 1000.0 * seconds_since(t0) / num_tracks);
    const int track_bytes = musly_track_size(setup);
    musly_jukebox_poweroff(setup);

    musly_jukebox* jb = musly_jukebox_poweron("timbre2", NULL);
    std::vector<musly_trackid> ids(num_tracks, -1);
    t0 = std::chrono::steady_clock::now();
    if (musly_jukebox_setmusicstyle(jb, tracks.data(), num_tracks) != 0) {
        std::fprintf(stderr, "setmusicstyle failed\n");
        return 1;
    }
    const double style_s = seconds_since(t0);
    t0 = std::chrono::steady_clock::now();
    if (musly_jukebox_addtracks(jb, tracks.data(), ids.data(), num_tracks, 1) != 0) {
        std::fprintf(stderr, "addtracks failed\n");
        return 1;
    }
    const double add_s = seconds_since(t0);
    std::printf("setmusicstyle:        %.3f s\n", style_s);
    std::printf("addtracks:            %.3f s (%.1f us/pair, %lld pairs)\n",
            add_s, 1e6 * add_s / ((double)num_tracks * num_tracks),
            (long long)num_tracks * num_tracks);
    std::printf("full jukebox build:   %.3f s\n", style_s + add_s);

    std::vector<float> sims(num_tracks);
    t0 = std::chrono::steady_clock::now();
    if (musly_jukebox_similarity(jb, tracks[0], ids[0], tracks.data(),
            ids.data(), num_tracks, sims.data()) != 0) {
        std::fprintf(stderr, "similarity failed\n");
        return 1;
    }
    const double query_s = seconds_since(t0);
    std::printf("one similarity query: %.3f s (%.1f us/dist)\n",
            query_s, 1e6 * query_s / num_tracks);

    const char* jbox_path = "/tmp/musly_baseline.jbox";
    t0 = std::chrono::steady_clock::now();
    if (musly_jukebox_tofile(jb, jbox_path) < 0) {
        std::fprintf(stderr, "tofile failed\n");
        return 1;
    }
    const double write_s = seconds_since(t0);
    FILE* fsz = fopen(jbox_path, "rb");
    fseek(fsz, 0, SEEK_END);
    const long jbox_bytes = ftell(fsz);
    fclose(fsz);
    std::printf("write jukebox:        %.3f s (%ld bytes, %.1f kB/track)\n",
            write_s, jbox_bytes, jbox_bytes / 1024.0 / num_tracks);
    std::printf("  of which track model size: %d bytes; mu/std only: 12 bytes\n",
            track_bytes);

    musly_jukebox_poweroff(jb);

    t0 = std::chrono::steady_clock::now();
    jb = musly_jukebox_fromfile(jbox_path);
    const double load_s = seconds_since(t0);
    if (!jb) {
        std::fprintf(stderr, "fromfile failed\n");
        return 1;
    }
    std::printf("load jukebox (full):  %.3f s\n", load_s);

    t0 = std::chrono::steady_clock::now();
    if (musly_jukebox_similarity(jb, tracks[1], ids[1], tracks.data(),
            ids.data(), num_tracks, sims.data()) != 0) {
        std::fprintf(stderr, "similarity after load failed\n");
        return 1;
    }
    std::printf("query after load:     %.3f s\n", seconds_since(t0));
    musly_jukebox_poweroff(jb);

    // Estimate collection I/O (deserialize N tracks from memory buffer)
    unsigned char* bin = new unsigned char[track_bytes];
    musly_jukebox* tmp = musly_jukebox_poweron("timbre2", NULL);
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < num_tracks; i++) {
        musly_track_tobin(tmp, tracks[i], bin);
        musly_track_frombin(tmp, bin, tracks[i]);
    }
    std::printf("collection serdes:    %.3f s (%d bytes/track, ~%.1f MB)\n",
            seconds_since(t0), track_bytes,
            track_bytes * (double)num_tracks / (1024.0 * 1024.0));
    musly_jukebox_poweroff(tmp);
    delete[] bin;

    for (int i = 0; i < num_tracks; i++) {
        musly_track_free(tracks[i]);
    }
    std::remove(jbox_path);
    return 0;
}
