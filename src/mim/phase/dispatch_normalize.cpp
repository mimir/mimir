#include "mim/phase/dispatch_normalize.h"

#include "mim/lam.h"

namespace mim {

std::pair<const Def*, const Def*> DispatchNormalize::finish_Lam(Lam*, const Def* filter, const Def* body) {
    if (auto dispatch = Dispatch(body))
        if (auto callee = normalize(dispatch); callee != dispatch.callee())
            body = new_world().app(callee, dispatch.arg());
    return {filter, body};
}

const Def* DispatchNormalize::normalize(Switch sw) {
    auto& w      = new_world();
    auto etas    = DefMap<const Def*>(); // one η-wrapper per distinct target keeps a uniform table a Pack
    auto targets = DefVec(sw.num_targets(), [&](size_t i) {
        auto target = sw.target(i);
        auto& eta   = etas[target];
        if (!eta) eta = eta_expand(target);
        return eta;
    });

    auto callee = w.extract(w.tuple(targets), sw.index());
    if (callee != sw.extract()) log().d("dispatch-normalize: {} → {}", sw.extract(), callee);
    return callee;
}

const Def* DispatchNormalize::eta_expand(const Def* f) {
    if (f->isa<Lam>()) return f;
    if (auto sw = Switch(f)) f = normalize(sw);
    return Lam::eta_expand(f);
}

} // namespace mim
