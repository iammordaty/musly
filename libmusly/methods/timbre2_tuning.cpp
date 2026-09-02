/**
 * Tuning variants for timbre2 (see eval/TUNING_RESULTS.md).
 *
 * All of them register with a negative priority: they must be selectable by
 * name but must never win the default-method lookup in plugins.cpp, which
 * picks the highest priority when no method is requested.
 */

#include "timbre2_cs.h"
#include "timbre2_cs_sh20.h"
#include "timbre2_cs_sh25.h"
#include "timbre2_sh20.h"
#include "timbre2_sh25.h"

namespace musly {
namespace methods {

MUSLY_METHOD_REGIMPL(timbre2_cs, -1);

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

MUSLY_METHOD_REGIMPL(timbre2_cs_sh20, -1);

timbre2_cs_sh20::timbre2_cs_sh20() :
        timbre(25, true, 1.0f, 0.20f, true)
{
}

const char*
timbre2_cs_sh20::about()
{
    return
        "timbre2_cs with covariance shrinkage lambda=0.20 instead of the\n"
        "default 0.10. Tuning experiment.";
}

MUSLY_METHOD_REGIMPL(timbre2_cs_sh25, -1);

timbre2_cs_sh25::timbre2_cs_sh25() :
        timbre(25, true, 1.0f, 0.25f, true)
{
}

const char*
timbre2_cs_sh25::about()
{
    return
        "timbre2_cs with covariance shrinkage lambda=0.25 instead of the\n"
        "default 0.10. Tuning experiment.";
}

// The lambda-only arms exist so the shrinkage effect can be read separately
// from the CSLS interaction. Shrinkage changes the stored features while CSLS
// is query-time only, so these share a collection with their _cs counterpart
// and are evaluated by cloning it (eval/clone_collection.py).

MUSLY_METHOD_REGIMPL(timbre2_sh20, -1);

timbre2_sh20::timbre2_sh20() :
        timbre(25, true, 1.0f, 0.20f, false)
{
}

const char*
timbre2_sh20::about()
{
    return
        "timbre2 with covariance shrinkage lambda=0.20 and no CSLS.\n"
        "Tuning experiment.";
}

MUSLY_METHOD_REGIMPL(timbre2_sh25, -1);

timbre2_sh25::timbre2_sh25() :
        timbre(25, true, 1.0f, 0.25f, false)
{
}

const char*
timbre2_sh25::about()
{
    return
        "timbre2 with covariance shrinkage lambda=0.25 and no CSLS.\n"
        "Tuning experiment.";
}

} /* namespace methods */
} /* namespace musly */
