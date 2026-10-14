#include "mim/phase/dispatch_normalize.h"

#include "mim/lam.h"

namespace mim {

const Def* DispatchNormalize::rewrite_mut_Lam(Lam* old_lam) {
    auto new_lam = new_world().mut_lam(rewrite(old_lam->type())->as<Pi>());
    map(old_lam, new_lam);
    if (!old_lam->is_set()) return new_lam;

    auto _      = enter(old_lam);
    auto filter = rewrite(old_lam->filter());
    auto body   = rewrite(old_lam->body());
    if (auto dispatch = Dispatch(body))
        if (auto callee = normalize(dispatch); callee != dispatch.callee())
            body = new_world().app(callee, dispatch.arg());
    new_lam->set(filter, body);
    return seal(old_lam, new_lam);
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
