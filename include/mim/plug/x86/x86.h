#pragma once

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include <mim/target.h>

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

inline bool is_mem(const Def* def) {
    auto type = def->type(); // Level has no type
    return type && Axm::isa<core::M>(type);
}

/// The register `r` of `x86.in r v` in hardware encoding order.
inline size_t in_reg(const Def* in) { return mim::Target::in_reg(in); }

/// The `f` of `mem.sched f m x`.
inline const Def* sched_insn(const App* app) { return mim::Target::sched_insn(app); }

/// The `v` of `x86.in r v` or `x86.fin r v`, otherwise @p def.
const Def* strip(const Def* def);

bool is_sched_cc(const App* app);

/// The `mem.sched` of an `x86.cc` that yields @p cond, if any.
const App* isa_sched_cc(const Def* cond);

/// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
const Def* prev_mem(const App* app);

} // namespace mim::plug::x86
