/**
 * Copyright 2013-2014, Dominik Schnitzer <dominik@schnitzer.at>
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "resampler.h"

#include <cmath>

#include "minilog.h"

namespace musly {

resampler::resampler(int input_rate, int output_rate):
        input_rate(input_rate),
        output_rate(output_rate),
        resample_factor((double)output_rate/(double)input_rate)
{
    libresample = resample_open(1, resample_factor, resample_factor);
}

resampler::~resampler()
{
    resample_close(libresample);
}

std::vector<float> resampler::resample(
        float* pcm_input,
        int pcm_len)
{
    std::vector<float> pcm_out(pcm_len*resample_factor);

    int srclen = 4096;
    int dstlen = (srclen*resample_factor + 1000);
    float* dst = new float[dstlen];

    int in_pos = 0;
    int out_pos = 0;
    float peak = 1.0f;

    while (in_pos < pcm_len) {

        int block_len = std::min(srclen, pcm_len-in_pos);
        int is_last_iteration = (in_pos+block_len == pcm_len);

        int input_read = 0;
        int out_written = resample_process(libresample, resample_factor,
                pcm_input+in_pos, block_len, is_last_iteration,
                &input_read, dst, dstlen);

        if (pcm_out.size() < (size_t)(out_pos+out_written)) {
            pcm_out.resize(out_pos+out_written);
        }
        for (int i = 0; i < out_written; i++) {
            pcm_out[out_pos + i] = dst[i];
            const float mag = std::fabs(dst[i]);
            if (std::isfinite(mag) && (mag > peak)) {
                peak = mag;
            }
        }

        in_pos += input_read;
        out_pos += out_written;
    }

    if (out_pos < (int)pcm_out.size()) {
        pcm_out.resize(out_pos);
    }

    // Keep the [-1, 1] output contract by attenuating uniformly instead of
    // clipping sample by sample. Decoded mp3 and resampler ringing routinely
    // overshoot full scale, and hard clipping here would distort the signal
    // before the scale-invariant RMS normalization in powerspectrum can run,
    // making features depend on input gain.
    if (peak > 1.0f) {
        const float scale = 1.0f / peak;
        for (size_t i = 0; i < pcm_out.size(); i++) {
            pcm_out[i] *= scale;
        }
    }

    delete[] dst;

    return pcm_out;
}


} /* namespace musly */
