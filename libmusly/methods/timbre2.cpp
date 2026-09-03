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

#include "timbre2.h"

namespace musly {
namespace methods {

/** Register timbre2 as the default method (priority 2).
 */
MUSLY_METHOD_REGIMPL(timbre2, 2);

timbre2::timbre2() :
        timbre(25, true, 0.15f)
{
}

const char*
timbre2::about()
{
    return
        "An improved timbre music similarity measure. Builds on 'timbre'\n"
        "by stacking 25 MFCCs with first-order temporal deltas (regression\n"
        "window ±2 frames). Tracks are represented as a single Gaussian\n"
        "over the stacked features and compared with the Jensen-Shannon\n"
        "divergence, normalized by Mutual Proximity.\n"
        "Deltas introduce temporal information absent from bag-of-frames\n"
        "timbre models (Mandel-Ellis / classic Musly timbre). The covariance\n"
        "estimate uses a Ledoit-Wolf shrinkage intensity of 0.15, which keeps\n"
        "the 50-dimensional Gaussians from becoming so sharp that a few tracks\n"
        "turn into hubs.";
}

} /* namespace methods */
} /* namespace musly */
