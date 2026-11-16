#pragma once

#include <algorithm>
#include <array>
#include <ranges>

#include <mim/plug/core/core.h>

#include "mim/plug/x86/autogen.h"

/// Prefix for this backend's diagnostics; concatenate it with the format literal.
#define MIM_X86_BE "x86 backend: "

namespace mim::plug::x86 {

/// General-purpose registers in hardware encoding order.
enum Reg : size_t { Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Num_Regs };

/// System V AMD64 ABI.
constexpr std::array<size_t, 6> Arg_Regs          = {Rdi, Rsi, Rdx, Rcx, R8, R9};
constexpr std::array<bool, Num_Regs> Caller_Saved = {1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
constexpr bool reserved(size_t r) { return r == Rsp || r == Rbp; }
constexpr size_t Num_Caller_Saved = std::ranges::count(Caller_Saved, true);
constexpr size_t Num_Allocatable = Num_Regs - size_t(std::ranges::count_if(std::views::iota(0uz, +Num_Regs), reserved));

inline bool is_mem(const Def* def) {
    auto type = def->type(); // Level has no type
    return type && Axm::isa<core::M>(type);
}

/// The register `r` of `x86.in r v` in hardware encoding order.
inline size_t in_reg(const Def* in) {
    return *Lit::isa(in->as<App>()->callee()->as<App>()->callee()->as<App>()->arg());
}

/// The `v` of `x86.in r v`, otherwise @p def.
inline const Def* strip(const Def* def) {
    if (auto in = Axm::isa<x86::in>(def)) return in->arg();
    return def;
}

/// The `f` of `x86.sched f m x`.
inline const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }

inline bool is_sched_cc(const App* app) {
    if (Axm::isa<sched>(app))
        if (auto f = sched_insn(app)->isa<App>(); f && f->axm()) return f->axm()->flags() == Annex::Base<cc>;
    return false;
}

/// The `x86.sched` of an `x86.cc` that yields @p cond, if any.
inline const App* isa_sched_cc(const Def* cond) {
    if (auto ex = cond->isa<Extract>())
        if (auto app = ex->tuple()->isa<App>(); app && is_sched_cc(app)) return app;
    return nullptr;
}

/// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
inline const Def* prev_mem(const App* app) {
    if (Axm::isa<sched>(app)) return app->callee()->as<App>()->arg();
    if (Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<slot>(app) || Axm::isa<rm>(app) || Axm::isa<mr>(app))
        return app->arg(app->num_args(), 0);
    return nullptr;
}

} // namespace mim::plug::x86
