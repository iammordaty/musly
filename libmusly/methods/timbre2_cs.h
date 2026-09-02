#ifndef MUSLY_METHODS_TIMBRE2_CS_H_
#define MUSLY_METHODS_TIMBRE2_CS_H_

#include "timbre.h"

namespace musly {
namespace methods {

class timbre2_cs : public timbre
{
MUSLY_METHOD_REGCLASS(timbre2_cs);

public:
    timbre2_cs();
    virtual const char* about();
};

} /* namespace methods */
} /* namespace musly */
#endif
