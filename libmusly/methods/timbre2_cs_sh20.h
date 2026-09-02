#ifndef MUSLY_METHODS_TIMBRE2_CS_SH20_H_
#define MUSLY_METHODS_TIMBRE2_CS_SH20_H_

#include "timbre.h"

namespace musly {
namespace methods {

class timbre2_cs_sh20 : public timbre
{
MUSLY_METHOD_REGCLASS(timbre2_cs_sh20);

public:
    timbre2_cs_sh20();
    virtual const char* about();
};

} /* namespace methods */
} /* namespace musly */
#endif
