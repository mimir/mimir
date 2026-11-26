#pragma once

#include <utility>

#include <mim/axm.h>

#include "mim/plug/mach/autogen.h"

namespace mim::plug::mach {

/// The hardware encoding of the register @p r of a backend's module `reg`, whose subtags are in encoding order.
template<class Reg>
constexpr size_t encoding(Reg r) {
    return Annex::flags2sub(std::to_underlying(r));
}

} // namespace mim::plug::mach
