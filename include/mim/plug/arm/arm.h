#pragma once

#include <array>

#include <mim/plug/core/core.h>
#include <mim/plug/mach/mach.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/arm/autogen.h"

namespace mim::plug::arm {

using mach::encoding;

constexpr size_t Num_Regs = Annex::num<reg>();

/// AAPCS64: arguments in x0 … x7, result in x0.
// clang-format off
constexpr std::array<size_t, 8> Arg_Regs = {
    encoding(reg::x0), encoding(reg::x1), encoding(reg::x2), encoding(reg::x3),
    encoding(reg::x4), encoding(reg::x5), encoding(reg::x6), encoding(reg::x7),
};
// clang-format on
constexpr size_t Ret_Reg              = encoding(reg::x0);
constexpr bool Caller_Saved[Num_Regs] = {
    true,  true,  true,  true,  true,  true,  true,  true,                // x0 - x7
    true,  true,  true,  true,  true,  true,  true,  true,                // x8 - x15
    false, false, false,                                                  // x16 - x18: scratch, reserved
    false, false, false, false, false, false, false, false, false, false, // x19 - x28: callee-saved
    false, false,                                                         // x29 fp, x30 lr
};
/// `x16`/`x17`/`x18` are the scratch and platform registers, `x29`/`x30` the frame pointer and link register.
constexpr bool reserved(size_t r) {
    return r == encoding(reg::x16) || r == encoding(reg::x17) || r == encoding(reg::x18) || r == encoding(reg::x29)
        || r == encoding(reg::x30);
}

using mem::sched;
using mem::strip_ptr_cast;

/// The `arm::csel` instance that selects on @p c.
constexpr flags_t cc_to_csel(cc c) { return Annex::Base<csel> | Annex::flags2sub(std::to_underlying(c)); }

} // namespace mim::plug::arm
