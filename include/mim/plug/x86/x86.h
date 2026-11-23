#pragma once

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/autogen.h"

/// Prefix for this backend's diagnostics; concatenate it with the format literal.
#define MIM_X86_BE "x86 backend: "

namespace mim::plug::x86 {

/// The hardware encoding of @p r.
constexpr size_t encoding(reg r) { return Annex::flags2sub(std::to_underlying(r)); }

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
constexpr size_t Num_Caller_Saved = std::ranges::count(Caller_Saved, true);
constexpr size_t Num_Allocatable = Num_Regs - size_t(std::ranges::count_if(std::views::iota(0uz, +Num_Regs), reserved));

using mem::sched;
using mem::strip_ptr_cast;

} // namespace mim::plug::x86
