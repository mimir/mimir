#include "mim/phase/ds2cps.h"

namespace mim {

std::pair<const Def*, const Def*> DS2CPS::rewrite_ops(Lam* old_lam, Lam* new_lam) {
    auto scope          = Scope(*this, Lam::isa_cn(new_lam));
    auto [filter, body] = RWPhase::rewrite_ops(old_lam, new_lam);
    return {filter, wire(scope.base(), body)};
}

const Def* DS2CPS::lift(const Def* k, Defs new_args, const Def* new_type, bool filter, Sym name) {
    auto& w   = new_world();
    auto cont = w.mut_con(new_type)->set_filter(filter);
    auto res  = cont->var();
    cont->set(name ? name : w.append_suffix(k->sym(), "_cont"));
    bind_new(cont->has_var());
    pending_.emplace_back(k, DefVec(new_args.begin(), new_args.end()), cont);
    log().d("lift {} ({}, {})", k, fe::Join(new_args, ", "), cont);
    return res;
}

const Def* DS2CPS::wire(size_t base, const Def* body) {
    auto& w = new_world();
    while (pending_.size() > base) {
        auto [callee, args, cont] = std::move(pending_.back());
        pending_.pop_back();
        cont->set_body(body);
        args.emplace_back(cont);
        body = w.app(callee, args);
    }
    return body;
}

} // namespace mim
