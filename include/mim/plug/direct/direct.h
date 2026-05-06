#pragma once

#include <mim/rewrite.h>
#include <mim/world.h>

#include "mim/plug/direct/autogen.h"

namespace mim::plug::direct {

/// @name %%direct.cps2ds_dep
/// ```
/// let k: Cn [t: T, Cn U t] = ...;
/// let f: [t: T] → U = %direct.cps2ds_dep (T, lm (t': T): * = [t → t']U) k;
/// ```
///@{
inline const Def* op_cps2ds_dep(const Def* k) {
    auto& w   = k->world();
    auto K    = Pi::isa_cn(k->type());
    auto T    = K->dom(2, 0);
    auto U    = Pi::isa_cn(K->dom(2, 1))->dom();
    auto l    = w.mut_lam(T, w.type())->set("Uf");
    auto body = U;

    if (auto [dom, var] = K->dom()->isa_binder<Sigma>(); dom)
        body = VarRewriter(var, l->var()).rewrite(U); // TODO typeof(dom->var()) != typeof(l->var())
    l->set(true, body);

    return w.app(w.app(w.annex<direct::cps2ds_dep>(), {T, l}), k);
}
///@}

} // namespace mim::plug::direct
