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

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <limits>

#include "musly/musly_types.h"
#include "mutualproximity.h"

namespace musly {

mutualproximity::mutualproximity(method* m) :
        m(m)
{
}

mutualproximity::~mutualproximity()
{
    // clear cache
    new_cache(0);
}

void
mutualproximity::new_cache(int size)
{
    for (int i = 0; i < (int)norm_tracks.size(); i++) {
        delete[] norm_tracks[i];
    }

    norm_tracks.clear();
    for (int i = 0; i < size; i++) {
        norm_tracks.push_back(m->track_alloc());
    }
}

int
mutualproximity::set_normtracks(
        musly_track** tracks,
        int length)
{
    if (length < 2) {
        return -1;
    }

    new_cache(length);

    int track_size = m->track_getsize();
    for (int i = 0; i < length; i++) {
        std::copy(tracks[i], tracks[i]+track_size, norm_tracks[i]);
    }

    return 0;
}

std::vector<musly_track*>*
mutualproximity::get_normtracks()
{
    return &norm_tracks;
}

void
mutualproximity::append_normfacts(
        int count) {
    norm_facts.resize(norm_facts.size() + count);
}

void
mutualproximity::set_normfacts(
        int position,
        Eigen::VectorXf& sim)
{
    // Ignore non-finite distances when estimating the reference distribution.
    int n = 0;
    double sum = 0.0;
    for (int i = 0; i < (int)sim.size(); i++) {
        if (std::isfinite(sim(i))) {
            sum += sim(i);
            n++;
        }
    }
    if (n < 2) {
        set_normfacts(position, 0.0f, 1.0f);
        return;
    }
    double mu = sum / static_cast<double>(n);
    double var = 0.0;
    for (int i = 0; i < (int)sim.size(); i++) {
        if (std::isfinite(sim(i))) {
            double d = sim(i) - mu;
            var += d * d;
        }
    }
    var /= (static_cast<double>(n) - 1.0);
    float stdv = static_cast<float>(std::sqrt(std::max(var, 0.0)));
    // Floor std to avoid division by zero in normalize().
    if (!(stdv > 0.0f) || !std::isfinite(stdv)) {
        stdv = 1.0f;
    }
    set_normfacts(position, static_cast<float>(mu), stdv);
}

void
mutualproximity::set_normfacts(
        int position,
        float mu,
        float std) {
    // allocate space if needed
    // (ideally, this has already been taken care of by append_normfacts;
    // resizing here is not safe under concurrent writers)
    if (position >= (int)norm_facts.size()) {
        norm_facts.resize(position+1);
    }
    if (!(std > 0.0f) || !std::isfinite(std)) {
        std = 1.0f;
    }
    if (!std::isfinite(mu)) {
        mu = 0.0f;
    }
    norm_facts[position].mu = mu;
    norm_facts[position].std = std;
}

void
mutualproximity::get_normfacts(
        int position,
        float* mu,
        float* std) {
    *mu = norm_facts[position].mu;
    *std = norm_facts[position].std;
}

void
mutualproximity::swap_normfacts(
        int position1,
        int position2) {
    std::swap(norm_facts[position1], norm_facts[position2]);
}

void
mutualproximity::trim_normfacts(
        int count) {
    norm_facts.resize(norm_facts.size() - count);
}

double
mutualproximity::normcdf(double x)
{
    // constants
    const double a1 =  0.254829592;
    const double a2 = -0.284496736;
    const double a3 =  1.421413741;
    const double a4 = -1.453152027;
    const double a5 =  1.061405429;
    const double p  =  0.3275911;

    // Save the sign of x
    int sign = 1;
    if (x < 0) {
        sign = -1;
    }
    x = fabs(x)/M_SQRT2;

    // A&S formula 7.1.26
    double t = 1.0/(1.0 + p*x);
    double y = 1.0 - (((((a5*t + a4)*t) + a3)*t + a2)*t + a1)*t*exp(-x*x);

    return 0.5*(1.0 + sign*y);
}

int
mutualproximity::normalize(
        int seed_position,
        int* other_positions,
        int length,
        float* sim)
{
    if (seed_position < 0 || seed_position >= (int)norm_facts.size()) {
        return -1;
    }
    float seed_mu = norm_facts[seed_position].mu;
    float seed_std = norm_facts[seed_position].std;
    if (!(seed_std > 0.0f)) {
        seed_std = 1.0f;
    }
    for (int i = 0; i < length; i++) {
        int pos = other_positions[i];
        if (pos < 0 || pos >= (int)norm_facts.size()) {
            return -1;
        }
        if (pos == seed_position) {
            sim[i] = 0;
            continue;
        }

        float d = sim[i];
        if (!std::isfinite(d)) {
            sim[i] = 1.0f;
            continue;
        }

        float other_std = norm_facts[pos].std;
        if (!(other_std > 0.0f)) {
            other_std = 1.0f;
        }
        double p1 = 1 - normcdf((d - seed_mu)/seed_std);
        double p2 = 1 - normcdf((d - norm_facts[pos].mu)/other_std);
        sim[i] = 1 - p1*p2;
    }
    return 0;
}




} /* namespace musly */
