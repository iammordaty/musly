#ifndef MUSLY_METHODS_TIMBRE2_SH20_H_
#define MUSLY_METHODS_TIMBRE2_SH20_H_

#include "timbre.h"

namespace musly {
namespace methods {

class timbre2_sh20 : public timbre
{
MUSLY_METHOD_REGCLASS(timbre2_sh20);

public:
    timbre2_sh20();
    virtual const char* about();
};

} /* namespace methods */
} /* namespace musly */
#endif
