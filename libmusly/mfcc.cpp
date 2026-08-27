/**
 * Copyright 2013-2014, Dominik Schnitzer <dominik@schnitzer.at>
 *                2026, Musly maintainers
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cmath>
#include "minilog.h"
#include "mfcc.h"

namespace musly {

mfcc::mfcc(int mel_bins, int mfcc_bins) :
        dct(mel_bins, mfcc_bins)
{
}

Eigen::MatrixXf mfcc::from_melspectrum(const Eigen::MatrixXf& mel)
{
    MINILOG(logTRACE) << "Computing MFCCs.";

    // Explicit log floor at -80 dB relative to unit power, independent of
    // input scaling. floor = 10^(-80/10) = 1e-8.
    const float log_floor = 1e-8f;
    Eigen::MatrixXf mel_clamped = mel.cwiseMax(log_floor);
    Eigen::MatrixXf mfcc_coeffs = dct.compress(mel_clamped.array().log());
    MINILOG(logTRACE) << "MFCCS: " << mfcc_coeffs;

    MINILOG(logTRACE) << "Finished Computing MFCCs.";
    return mfcc_coeffs;
}

} /* namespace musly */
