/**
 * Copyright 2013-2014, Dominik Schnitzer <dominik@schnitzer.at>
 *                2014, Jan Schlueter <jan.schlueter@ofai.at>
 *                2026, Musly maintainers
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <Eigen/Core>

#include "minilog.h"
#include "windowfunction.h"
#include "timbre.h"


namespace musly {
namespace methods {

/** Register timbre with musly with priority (1)
 */
MUSLY_METHOD_REGIMPL(timbre, 1);



timbre::timbre() :
        timbre(25, false)
{
}

timbre::timbre(int mfcc_bins_, bool use_deltas_) :

        // initialize method configuration parameters
        sample_rate(22050),
        window_size(1024),
        hop(0.5f),
        max_pcmlength(4*60*sample_rate),
        ps_bins(window_size/2+1),
        mel_bins(36),
        mfcc_bins(mfcc_bins_),
        use_deltas(use_deltas_),
        delta_width(2),
        num_segments(3),
        feature_dim(use_deltas_ ? mfcc_bins_ * 2 : mfcc_bins_),
        focus_fraction(0.6f),
        min_analysis_length(window_size
                + (feature_dim + 1) * (int)(window_size * hop)),

        // spectra and filters
        ps(windowfunction::hann(window_size), hop),
        mel(ps_bins, mel_bins, sample_rate),
        mfccs(mel_bins, mfcc_bins_),
        gs(feature_dim),
        mp(this)
{
    // Configure the musly_track features and save the musly_track offsets

    // the feature mean
    track_mu = track_addfield_floats("gaussian.mu", gs.get_dim());
    // add the covariance (symmetric matrix)
    track_covar = track_addfield_floats("gaussian.covar", gs.get_covarelems());
    // add the log(det(covar)) of the covariance for performance reasons
    track_logdet = track_addfield_floats("gaussian.covar_logdet", 1);

    // React on changes to the trackid mapping in the ordered_idpool
    idpool.set_observer(this);
}

timbre::~timbre()
{
}

const char*
timbre::about()
{
    return
        "A timbre only music similarity measure based 'mandelellis'. It\n"
        "improves the basic measure in multiple ways to achieve superior\n"
        "results:\n"
        "We compute a single Gaussian representation from the songs\n"
        "using 25 MFCCs. The similarity between two tracks is computed\n"
        "with the Jensen-Shannon divergence. The Similarities are\n"
        "normalized with Mutual Proximity:\n"
        "D. Schnitzer et al.: Using mutual proximity to improve\n"
        "content-based audio similarity. In the proceedings of the 12th\n"
        "International Society for Music Information Retrieval\n"
        "Conference, ISMIR, 2011.";
}

Eigen::MatrixXf
timbre::compute_deltas(const Eigen::MatrixXf& mfcc_frames) const
{
    // Regression deltas over ±delta_width frames (HTK-style).
    const int w = delta_width;
    const int T = mfcc_frames.cols();
    const int D = mfcc_frames.rows();
    Eigen::MatrixXd deltas = Eigen::MatrixXd::Zero(D, T);
    double denom = 0.0;
    for (int n = 1; n <= w; n++) {
        denom += n * n;
    }
    denom *= 2.0;

    Eigen::MatrixXd frames = mfcc_frames.cast<double>();
    for (int t = 0; t < T; t++) {
        Eigen::VectorXd num = Eigen::VectorXd::Zero(D);
        for (int n = 1; n <= w; n++) {
            int tp = std::min(T - 1, t + n);
            int tm = std::max(0, t - n);
            num += n * (frames.col(tp) - frames.col(tm));
        }
        deltas.col(t) = num / denom;
    }
    return deltas.cast<float>();
}

Eigen::MatrixXf
timbre::select_frames(
        const Eigen::MatrixXf& features,
        const Eigen::MatrixXf& power_spectrum) const
{
    // Frame energy from power spectrum; keep frames within 60 dB of P95.
    Eigen::VectorXf energy = power_spectrum.colwise().sum();
    if (energy.size() == 0 || features.cols() == 0) {
        return Eigen::MatrixXf(0, 0);
    }

    std::vector<float> sorted(energy.data(), energy.data() + energy.size());
    std::sort(sorted.begin(), sorted.end());
    float p95 = sorted[(size_t)((sorted.size() - 1) * 0.95)];
    if (!(p95 > 0.0f) || !std::isfinite(p95)) {
        return features;
    }
    float threshold = p95 * 1e-6f; // −60 dB relative to P95

    int kept = 0;
    for (int i = 0; i < energy.size() && i < features.cols(); i++) {
        if (energy(i) >= threshold) {
            kept++;
        }
    }
    if (kept <= feature_dim) {
        // Too few frames after gating — keep all to allow estimation.
        return features;
    }

    Eigen::MatrixXf selected(features.rows(), kept);
    int out = 0;
    for (int i = 0; i < energy.size() && i < features.cols(); i++) {
        if (energy(i) >= threshold) {
            selected.col(out++) = features.col(i);
        }
    }
    return selected;
}

int
timbre::analyze_track(
        float* pcm,
        int length,
        musly_track* track)
{
    MINILOG(logTRACE) << "T analysis started. samples=" << length;

    if (length < window_size) {
        MINILOG(logTRACE) << "T analysis failed: input too short.";
        return 2;
    }

    // Restrict the analysis to the centered part of the signal. Intros and
    // outros are rarely representative — extended electronic mixes routinely
    // open and close with beats only. Scaling with the signal length keeps
    // different edits of the same track structurally aligned.
    int focus_length = (int)(length * focus_fraction);
    if ((focus_length >= min_analysis_length) && (focus_length < length)) {
        pcm += (length - focus_length) / 2;
        length = focus_length;
        MINILOG(logTRACE) << "T focusing on centered part. samples=" << length;
    }

    // Collect frames from several evenly spaced segments for long signals.
    // For short signals, analyze the whole PCM once.
    int segment_len = max_pcmlength / num_segments;
    if (segment_len < window_size) {
        segment_len = window_size;
    }

    std::vector<Eigen::MatrixXf> feature_blocks;
    std::vector<Eigen::MatrixXf> power_blocks;
    int total_frames = 0;

    auto analyze_chunk = [&](int start, int chunk_len) {
        if (chunk_len < window_size) {
            return;
        }
        Eigen::Map<Eigen::VectorXf> pcm_vector(pcm + start, chunk_len);
        Eigen::MatrixXf power_spectrum = ps.from_pcm(pcm_vector);
        if (power_spectrum.cols() == 0) {
            return;
        }
        Eigen::MatrixXf mel_spectrum = mel.from_powerspectrum(power_spectrum);
        Eigen::MatrixXf mfcc_representation =
                mfccs.from_melspectrum(mel_spectrum);
        if (use_deltas) {
            Eigen::MatrixXf deltas = compute_deltas(mfcc_representation);
            Eigen::MatrixXf combined(feature_dim, mfcc_representation.cols());
            combined.topRows(mfcc_bins) = mfcc_representation;
            combined.bottomRows(mfcc_bins) = deltas;
            feature_blocks.push_back(combined);
        } else {
            feature_blocks.push_back(mfcc_representation);
        }
        power_blocks.push_back(power_spectrum);
        total_frames += feature_blocks.back().cols();
    };

    if (length <= max_pcmlength) {
        analyze_chunk(0, length);
    } else {
        // Evenly spaced segments spanning the whole file.
        for (int s = 0; s < num_segments; s++) {
            int start = (int)((long long)s * (length - segment_len)
                    / std::max(1, num_segments - 1));
            if (start < 0) {
                start = 0;
            }
            if (start + segment_len > length) {
                start = length - segment_len;
            }
            analyze_chunk(start, segment_len);
        }
    }

    if (total_frames == 0 || feature_blocks.empty()) {
        MINILOG(logTRACE) << "T analysis failed: no frames.";
        return 2;
    }

    Eigen::MatrixXf all_features(feature_dim, total_frames);
    Eigen::MatrixXf all_power(ps_bins, total_frames);
    int col = 0;
    for (size_t b = 0; b < feature_blocks.size(); b++) {
        int n = feature_blocks[b].cols();
        all_features.block(0, col, feature_dim, n) = feature_blocks[b];
        // power_blocks may have different row counts only if FFT size differs —
        // it does not; align by taking min cols already matched above.
        int pn = std::min(n, (int)power_blocks[b].cols());
        all_power.block(0, col, ps_bins, pn) = power_blocks[b].leftCols(pn);
        if (pn < n) {
            // pad remaining with last column energy proxy
            for (int k = pn; k < n; k++) {
                all_power.col(col + k) = power_blocks[b].col(pn - 1);
            }
        }
        col += n;
    }

    Eigen::MatrixXf selected = select_frames(all_features, all_power);
    if (selected.cols() == 0) {
        selected = all_features;
    }

    // estimate the Gaussian from the MFCC (or MFCC+delta) representation
    gaussian g = {0, 0, 0, 0};
    g.mu = &track[track_mu];
    g.covar = &track[track_covar];
    g.covar_logdet = &track[track_logdet];
    if (gs.estimate_gaussian(selected, g) == false) {
        MINILOG(logTRACE) << "T Gaussian model estimation failed.";
        return 2;
    }

    MINILOG(logTRACE) << "T analysis finished!";

    return 0;
}


void
timbre::similarity_raw(
        musly_track* track,
        musly_track** tracks,
        int length,
        float* similarities)
{
    // map seed track to gaussian structure
    gaussian g0 = {0, 0, 0, 0};
    g0.mu = &track[track_mu];
    g0.covar = &track[track_covar];
    g0.covar_logdet = &track[track_logdet];

    // create the temporary buffer required for the Jensen-Shannon divergence
    musly_track* tmp_t = track_alloc();
    gaussian tmp = {0, 0, 0, 0};
    tmp.mu = &tmp_t[track_mu];
    tmp.covar = &tmp_t[track_covar];
    tmp.covar_logdet = &tmp_t[track_logdet];

    // iterate over all musly_tracks to compute the Jensen-Shannon divergence
    for (int i = 0; i < length; i++) {
        gaussian gi = {0, 0, 0, 0};
        musly_track* track1 = tracks[i];
        gi.mu = &track1[track_mu];
        gi.covar = &track1[track_covar];
        gi.covar_logdet = &track1[track_logdet];

        similarities[i] = gs.jensenshannon(g0, gi, tmp);
    }

    delete[] tmp_t;
}



int
timbre::similarity(
        musly_track* track,
        musly_trackid seed_trackid,
        musly_track** tracks,
        musly_trackid* trackids,
        int length,
        float* similarities)
{
    if ((length <= 0) || !track || ! tracks || !similarities) {
        return -1;
    }

    // compute raw similarities
    similarity_raw(track, tracks, length, similarities);

    // normalize with mp
    // - lookup positions of trackids in the ordered_idpool
    int seed_position = idpool.position_of(seed_trackid);
    int* other_positions = new int[length];
    for (int i = 0; i < length; i++) {
        other_positions[i] = idpool.position_of(trackids[i]);
    }
    // - call mp.normalize with these positions
    int res = mp.normalize(seed_position, other_positions, length, similarities);
    delete[] other_positions;

    return res;
}

int
timbre::set_musicstyle(
            musly_track** tracks,
            int length)
{
    MINILOG(logTRACE) << "T initializing mutual proximity!";

    // save the mp normalization tracks
    return mp.set_normtracks(tracks, length);
}

int
timbre::add_tracks(
        musly_track** tracks,
        musly_trackid* trackids,
        int length,
        bool generate_ids) {
    if (mp.get_normtracks()->size() == 0) {
        return -1;  // not initialized, cannot add tracks
    }
    int num_new;
    if (generate_ids) {
        idpool.generate_ids(trackids, length);
        num_new = length;
    }
    else {
        num_new = idpool.add_ids(trackids, length);
    }

    Eigen::VectorXf sim(mp.get_normtracks()->size());
    mp.append_normfacts(num_new);
    int pos = idpool.get_size() - length;
    for (int i = 0; i < length; i++) {
        similarity_raw(tracks[i], mp.get_normtracks()->data(),
                mp.get_normtracks()->size(), sim.data());

        mp.set_normfacts(pos + i, sim);
    }
    return 0;
}

void
timbre::remove_tracks(
        musly_trackid* trackids,
        int length) {
    length = idpool.move_to_end(trackids, length);
    mp.trim_normfacts(length);
    idpool.remove_last(length);
}

int
timbre::get_trackcount() {
    return idpool.get_size();
}

int
timbre::get_maxtrackid() {
    return idpool.get_max_seen();
}

int
timbre::get_trackids(
        musly_trackid* trackids) {
    std::copy(idpool.idlist().begin(), idpool.idlist().end(), trackids);
    return idpool.get_size();
}

void
timbre::swapped_positions(
        int pos_a,
        int pos_b) {
    // positions in idpool have changed; update mp index accordingly
    mp.swap_normfacts(pos_a, pos_b);
}

int
timbre::serialize_metadata(
        unsigned char* buffer) {
    if (buffer) {
        // number of registered tracks
        *(int*)(buffer) = idpool.get_size();
        buffer += sizeof(int);

        // largest seen track id
        *(musly_trackid*)(buffer) = idpool.get_max_seen();
        buffer += sizeof(musly_trackid);

        // mutual proximity tracks
        std::vector<musly_track*> &mptracks = *mp.get_normtracks();
        *(int*)(buffer) = mptracks.size();
        buffer += sizeof(int);
        for (int i = 0; i < (int)mptracks.size(); i++) {
            std::copy(mptracks[i], mptracks[i] + track_getsize(), (musly_track*)buffer);
            buffer += track_getsize() * sizeof(musly_track);
        }
    }
    return sizeof(int) + sizeof(musly_trackid) + sizeof(int)
            + mp.get_normtracks()->size() * track_getsize() * sizeof(musly_track);
}

int
timbre::deserialize_metadata(
        unsigned char* buffer) {
    // number of registered tracks
    int expected_tracks = *(int*)(buffer);
    buffer += sizeof(int);

    // largest seen track id
    musly_trackid max_seen = *(musly_trackid*)(buffer);
    buffer += sizeof(musly_trackid);
    idpool.add_ids(&max_seen, 1);
    idpool.remove_ids(&max_seen, 1);

    // mutual proximity tracks
    int num_mptracks = *(int*)(buffer);
    buffer += sizeof(int);
    musly_track** mptracks = new musly_track*[num_mptracks];
    for (int i = 0; i < num_mptracks; i++) {
        mptracks[i] = (musly_track*)buffer;
        buffer += track_getsize() * sizeof(musly_track);
    }
    mp.set_normtracks(mptracks, num_mptracks);
    delete[] mptracks;
    mp.append_normfacts(expected_tracks);

    return expected_tracks;
}

int
timbre::serialize_trackdata(
        unsigned char* buffer,
        int num_tracks,
        int skip_tracks) {
    if ((num_tracks < 0) || (skip_tracks < 0)) {
        return -1;
    }
    if (buffer) {
        if (num_tracks + skip_tracks > idpool.get_size()) {
            return -1;
        }
        for (int i = skip_tracks; i < skip_tracks + num_tracks; i++) {
            *(musly_trackid*)(buffer) = idpool[i];
            buffer += sizeof(musly_trackid);
            mp.get_normfacts(i,
                    (float*)(buffer),
                    (float*)(buffer + sizeof(float)));
            buffer += 2 * sizeof(float);
        }
    }
    return num_tracks * (sizeof(musly_trackid) + 2 * sizeof(float));
}

int
timbre::deserialize_trackdata(
        unsigned char* buffer,
        int num_tracks) {
    if (num_tracks < 0) {
        return -1;
    }
    int had_tracks = idpool.get_size();
    for (int i = 0; i < num_tracks; i++) {
        idpool.add_ids((musly_trackid*)buffer, 1);
        buffer += sizeof(musly_trackid);
        mp.set_normfacts(had_tracks + i,
                *(float*)(buffer),
                *(float*)(buffer + sizeof(float)));
        buffer += 2 * sizeof(float);
    }
    return num_tracks;
}

} /* namespace methods */
} /* namespace musly */
