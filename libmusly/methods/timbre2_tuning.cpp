/**
 * Tuning variants for timbre2 (see eval/TUNING_RESULTS.md).
 */

#include "timbre2_cs.h"

namespace musly {
namespace methods {

MUSLY_METHOD_REGIMPL(timbre2_cs, 3);

timbre2_cs::timbre2_cs() :
        timbre(25, true, 1.0f, 0.1f, true)
{
}

const char*
timbre2_cs::about()
{
    return
        "timbre2 with a CSLS-style hub penalty folded into the Mutual\n"
        "Proximity reference distributions. Tuning experiment.";
}

} /* namespace methods */
} /* namespace musly */
