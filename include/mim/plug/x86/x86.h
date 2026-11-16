#pragma once

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>

#include "mim/plug/x86/autogen.h"

/// Prefix for this backend's diagnostics; concatenate it with the format literal.
#define MIM_X86_BE "x86 backend: "

namespace mim::plug::x86 {

inline bool is_mem(const Def* def) {
    auto type = def->type(); // Level has no type
    return type && Axm::isa<core::M>(type);
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
