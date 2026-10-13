#pragma once

#include <mim/lam.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>

#include "mim/plug/affine/autogen.h"

namespace mim::plug::affine {

/// `affine.For (0, bound, 1, acc)` over a fresh `body: Cn [I64, acc_t, Cn acc_t]` named @p name.
/// @returns `{body, for_call}`.
inline std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

/// Nests one counting loop per bound of @p dims inside @p cur, threading @p cur, @p exit and @p acc down to
/// the innermost body; @returns the raw i64 loop counters.
inline DefVec counting_nest(Lam*& cur, const Def*& exit, const Def*& acc, Defs dims, std::string_view name) {
    auto& w    = cur->world();
    auto iters = DefVec();
    iters.reserve(dims.size());
    for (size_t i = 0, e = dims.size(); i != e; ++i) {
        auto bound                  = w.call<core::bitcast>(w.type_i64(), dims[i]);
        auto [body, for_call]       = counting_for(bound, acc, exit, w.sym(std::format("{}_{}", name, i)));
        auto [iter, new_acc, yield] = body->vars<3>();
        exit                        = yield;
        acc                         = new_acc;
        iters.emplace_back(iter);
        cur->set(true, for_call);
        cur = body;
    }
    return iters;
}

} // namespace mim::plug::affine
