#pragma once

#include <bit>
#include <optional>

#include <mim/axm.h>
#include <mim/lam.h>

#include "mim/plug/x86/autogen.h"
#include "mim/plug/x86/imm.h"

/// Machine facts about the instructions of the `x86` plugin, shared by `x86_isel.gen` and the backend.
namespace mim::plug::x86::insn {

/// A register-to-register copy, which move elimination makes free of latency.
inline constexpr u32 Copy = 1;
inline constexpr u32 Load = 4;

/// The operand whose register the result of a two-address instruction @p base takes.
inline std::optional<size_t> tied(flags_t base) {
    // clang-format off
    if (base == Annex::Base<add> || base == Annex::Base<sub> || base == Annex::Base<and_> || base == Annex::Base<or_>
     || base == Annex::Base<xor_> || base == Annex::Base<imul> || base == Annex::Base<shl> || base == Annex::Base<shr>
     || base == Annex::Base<sar>) return 0;
    if (base == Annex::Base<fadd> || base == Annex::Base<fsub> || base == Annex::Base<fmul>
     || base == Annex::Base<fdiv> || base == Annex::Base<fabs>) return 0;
    // clang-format on
    if (base == Annex::Base<cmovcc> || base == Annex::Base<fsel>) return 1;
    return {};
}

/// The operation `f` of the memory form `x86.rm f`, `x86.rm1 f`, or `x86.mr f` @p app.
inline flags_t op(const App* app) { return std::get<0>(Axm::get(app->callee()->as<App>()->arg()))->base(); }

/// The operand whose register the result of the two-address instruction @p app takes.
/// `x86.rm f (m, a, addr)` overwrites `a` if `f` overwrites its first operand.
inline std::optional<size_t> tied(const App* app) {
    auto base = app->axm()->base();
    if (base != Annex::Base<rm>) return tied(base);
    if (tied(op(app)) == 0) return 1;
    return {};
}

/// The latency in cycles of the instruction @p base on `s`-bit operands, roughly as on recent cores (uops.info).
inline u32 latency(flags_t base, nat_t s) {
    if (base == Annex::Base<imul>) return 3;
    if (base == Annex::Base<div> || base == Annex::Base<idiv>) return reg_width(s) == 64 ? 18 : 14;
    if (base == Annex::Base<fdiv>) return 11;
    if (base == Annex::Base<fsqrt>) return 14;
    if (base == Annex::Base<load> || base == Annex::Base<fload>) return Load;
    if (base == Annex::Base<slot>) return 0;
    return 1;
}

/// The cost of the instruction @p app: its latency plus one, so of two equally fast covers the shorter wins.
/// An address is no instruction and costs nothing.
inline u32 cost(const App* app) {
    auto axm  = app->axm();
    auto base = axm->base();
    if (Axm::isa<Addr>(app->type()) || base == Annex::Base<index>) return 0;
    auto s = [](const Def* x) { return Idx::isa_lit(x->type()).value_or(64); };
    if (base == Annex::Base<rm> || base == Annex::Base<mr>) {
        auto [m, x, y] = app->args<3>();
        auto res       = Load + latency(op(app), s(base == Annex::Base<rm> ? x : y)) + 1;
        return base == Annex::Base<mr> ? res + 1 : res;
    }
    if (base == Annex::Base<rm1>) return Load + latency(op(app), s(app->proj(2, 1))) + 1;
    if (base == Annex::Base<trunc>) {
        auto n = *Idx::isa_lit(app->type());
        if (n != 0 && !std::has_single_bit(n)) return latency(Annex::Base<div>, n) + 4;
        auto w = reg_width(n);
        return n == 0 || (w < 64 && n == 1_n << w) ? 0 : 2;
    }
    auto arg = app->arg();
    return latency(base, s(arg->num_projs() > 1 ? arg->proj(arg->num_projs(), arg->num_projs() - 1) : arg)) + 1;
}

} // namespace mim::plug::x86::insn
