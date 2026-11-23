#pragma once

#include <mim/liveness.h>

#include "mim/plug/arm/target.h"

namespace mim::plug::arm {

class Liveness : public mim::Liveness {
public:
    explicit Liveness(Lam* fun)
        : mim::Liveness(arm::target(), fun) {}
};

} // namespace mim::plug::arm
