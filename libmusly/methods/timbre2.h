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

#ifndef MUSLY_METHODS_TIMBRE2_H_
#define MUSLY_METHODS_TIMBRE2_H_

#include "timbre.h"

namespace musly {
namespace methods {

/** Timbre similarity with first-order MFCC deltas for temporal information.
 * Registered at priority 2 so it becomes the default method for musly -N.
 */
class timbre2 : public timbre
{
MUSLY_METHOD_REGCLASS(timbre2);

public:
    timbre2();

    virtual const char*
    about();
};

} /* namespace methods */
} /* namespace musly */
#endif /* MUSLY_METHODS_TIMBRE2_H_ */
