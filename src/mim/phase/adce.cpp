#include "mim/phase/adce.h"

namespace mim {

enum {
    Proxy_Dead, // proxy(var) <- var is optimistically assumed dead
};

/*
 * Analysis
 */

bool ADCE::Analysis::is_dead(const Def* var) const {
    auto l = lattice(var);
    return l && Proxy::isa<Proxy_Dead>(l);
}

const Def* ADCE::Analysis::rewrite_imm_App(const App* app) {
    if (auto res = rewrite_known(app)) return res;
    return Super::rewrite_imm_App(app);
}

DefVec ADCE::Analysis::join_args(Lam* lam, DefVec&) {
    return DefVec(lam->num_tvars(), [&](size_t i) -> const Def* {
        auto var = lam->tvar(i);
        if (auto l = lattice(var)) return l;
        auto dead = proxy(var->type(), {var}, Proxy_Dead)->set(var->dbg_key());
        return lattice(var, dead), dead;
    });
}

void ADCE::Analysis::reach(const Proxy* proxy) {
    if (proxy->tag() == Proxy_Dead && pin(proxy->op(0))) log().d("live: {}", proxy->op(0));
}

/*
 * Transformation
 */

Sieve ADCE::sieve(Lam* lam) {
    if (auto s = fe::lookup(lam2sieve_, lam)) return *s;

    auto n    = lam->num_tvars();
    auto keep = !lam->is_rewritable() || !lam->has_var()
                  ? Sieve(n)
                  : Sieve(lam->tvars(), [this](const Def* var) { return !analysis_.is_dead(var); });
    return lam2sieve_.emplace(lam, std::move(keep)).first->second;
}

Lam* ADCE::build_lam(Lam* old_lam) {
    if (auto new_lam = fe::lookup(lam_old2new_, old_lam)) return new_lam;

    invalidate();
    auto keep     = sieve(old_lam);
    auto n        = keep.num_old();
    auto new_doms = rewrite(keep.gather(old_lam->doms(n)));
    auto new_lam  = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
    log().d("{} → {}: {} of {} vars dead", old_lam, new_lam, n - keep.num_new(), n);
    profile_count("adce.vars.eliminated", n - keep.num_new());
    lam_old2new_[old_lam] = new_lam;
    lam_new2old_[new_lam] = old_lam;

    auto old_vars = old_lam->tvars();
    auto new_vars = DefVec(n, [&](size_t i) -> const Def* {
        if (auto j = keep[i]; j != Sieve::Gone) return new_lam->var(keep.num_new(), j)->set(old_vars[i]->dbg());
        return new_world().bot(rewrite(old_lam->dom(n, i)));
    });

    map(old_lam->var(), new_vars);
    auto _ = enter(old_lam);
    fill(old_lam, new_lam);
    return new_lam;
}

const Def* ADCE::rewrite_imm_App(const App* old_app) {
    auto old_lam = old_app->callee()->isa_mut<Lam>();
    // The callee may only fold to a rebuilt Lam in the new World, e.g. a branch whose condition becomes constant.
    if (!old_lam)
        if (auto new_lam = rewrite(old_app->callee())->isa_mut<Lam>()) old_lam = fe::lookup(lam_new2old_, new_lam);

    if (old_lam)
        if (auto keep = sieve(old_lam); !keep.all()) {
            auto new_lam = build_lam(old_lam);
            return new_world().app(new_lam, rewrite(keep.gather(old_app->targs())));
        }

    return Super::rewrite_imm_App(old_app);
}

const Def* ADCE::rewrite_mut_Lam(Lam* old_lam) {
    if (!sieve(old_lam).all()) return build_lam(old_lam);
    return Super::rewrite_mut_Lam(old_lam);
}

} // namespace mim
