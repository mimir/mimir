#pragma once

#include <array>

#include <mim/plug/core/core.h>
#include <mim/plug/mach/mach.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/autogen.h"

namespace mim::plug::x86 {

using mach::encoding;

constexpr size_t Num_Regs = Annex::num<reg>();
constexpr size_t Num_Xmm  = Annex::num<xmm>();

/// System V AMD64 ABI.
// clang-format off
constexpr std::array<size_t, 6> Arg_Regs = {
    encoding(reg::rdi), encoding(reg::rsi), encoding(reg::rdx),
    encoding(reg::rcx), encoding(reg::r8),  encoding(reg::r9),
};
// clang-format on
constexpr std::array<bool, Num_Regs> Caller_Saved = {1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
constexpr bool reserved(size_t r) { return r == encoding(reg::rsp) || r == encoding(reg::rbp); }

using mem::sched;
using mem::strip_ptr_cast;

} // namespace mim::plug::x86
