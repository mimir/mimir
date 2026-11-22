#pragma once

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include <mim/plug/core/core.h>

#include "mim/plug/x86/autogen.h"

/// Prefix for this backend's diagnostics; concatenate it with the format literal.
#define MIM_X86_BE "x86 backend: "

namespace mim::plug::x86 {

/// The hardware encoding of @p r.
constexpr size_t encoding(reg r) { return Annex::flags2sub(std::to_underlying(r)); }

constexpr size_t Num_Regs = Annex::num<reg>();
constexpr size_t Num_Fpr  = Annex::num<freg>();

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

inline bool is_mem(const Def* def) {
    auto type = def->type(); // Level has no type
    return type && Axm::isa<core::M>(type);
}

/// The register `r` of `x86.in r v` in hardware encoding order.
inline size_t in_reg(const Def* in) {
    return *Lit::isa(in->as<App>()->callee()->as<App>()->callee()->as<App>()->arg());
}

/// The `v` of `x86.in r v` or `x86.fin r v`, otherwise @p def.
inline const Def* strip(const Def* def) {
    if (auto in = Axm::isa<x86::in>(def)) return in->arg();
    if (auto in = Axm::isa<x86::fin>(def)) return in->arg();
    return def;
}

/// @p def without the casts between pointer types around it, which `mem.legalize_ptr` inserts and which are no-ops.
inline const Def* strip_ptr_cast(const Def* def) {
    while (auto cast = Axm::isa<core::bitcast>(def)) {
        if (!Axm::isa<core::Ptr>(def->type()) || !Axm::isa<core::Ptr>(cast->arg()->type())) break;
        def = cast->arg();
    }
    return def;
}

/// The `f` of `x86.sched f m x`.
inline const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }

inline bool is_sched_cc(const App* app) { return Axm::isa<sched>(app) && Axm::isa<cc, 1>(sched_insn(app)); }

/// The `x86.sched` of an `x86.cc` that yields @p cond, if any.
inline const App* isa_sched_cc(const Def* cond) {
    if (auto ex = cond->isa<Extract>())
        if (auto app = ex->tuple()->isa<App>(); app && is_sched_cc(app)) return app;
    return nullptr;
}

/// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
inline const Def* prev_mem(const App* app) {
    if (Axm::isa<sched>(app)) return app->callee()->as<App>()->arg();
    if (Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<slot>(app) || Axm::isa<rm>(app) || Axm::isa<mr>(app)
        || Axm::isa<fload>(app) || Axm::isa<fstore>(app))
        return app->arg(app->num_args(), 0);
    return nullptr;
}

} // namespace mim::plug::x86
