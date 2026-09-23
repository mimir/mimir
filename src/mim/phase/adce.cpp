#include "mim/phase/adce.h"

namespace mim {

enum {
    Proxy_Dead, // proxy(var) <- var is optimistically assumed dead
};

/// Does @p lam's signature refer to its own binder's Var?
/// Such a signature cannot be narrowed: dropping a component would tear its siblings off the binder.
static bool is_dependent(Lam* lam) { return lam->type()->isa_mut() || lam->type()->dom()->isa_mut(); }

/*
 * Analysis
 */

void ADCE::Analysis::reset() {
    Super::reset();
    visited_.clear();
}

bool ADCE::Analysis::is_dead(const Def* var) const {
    auto l = lattice(var);
    return l && Proxy::isa<Proxy_Dead>(l);
}

const Def* ADCE::Analysis::rewrite_imm_App(const App* app) {
    auto abstr_arg    = rewrite(app->arg());
    auto abstr_callee = rewrite(app->callee());

    if (auto lam = isa_optimizable(abstr_callee->isa_mut<Lam>())) {
        auto n          = lam->num_tvars();
        auto dependent  = is_dependent(lam);
        auto abstr_vars = DefVec(n, [&](size_t i) -> const Def* {
            auto var = lam->tvar(i);
            if (dependent) return pin(var), var;
            if (auto l = lattice(var)) return l;
            auto dead = world().proxy(var->type(), {var}, Proxy_Dead)->set(var->dbg_key());
            return lattice(var, dead), dead;
        });

        if (n > 1) lattice(lam->var(), world().tuple(abstr_vars));
        // raw_app: World::app would β-reduce or partially evaluate lam.
        return world().raw_app(rewrite(app->type()), lam, abstr_arg);
    }

    return Super::rewrite_imm_App(app);
}

void ADCE::Analysis::finalize() {
    for (auto def : world().roots())
        analyze(def);
}

void ADCE::Analysis::analyze(const Def* def) {
    // A Var's lattice spells all of its projections; the live ones already reach us via their Extract.
    if (def->isa<Var>()) return;
    if (auto [_, ins] = visited_.emplace(def); !ins) return;
    if (auto l = lookup(def); l && l != def) return analyze(l);

    if (auto dead = def->isa<Proxy>()) {
        if (dead->tag() == Proxy_Dead && pin(dead->op(0))) log().d("live: {}", dead->op(0));
        return; // never walk a proxy's deps: its var would look live
    }

    if (auto app = def->isa<App>()) {
        if (auto lam = isa_optimizable(app->callee()->isa_mut<Lam>())) {
            analyze(app->type());

            auto n = lam->num_tvars();
            for (size_t i = 0; i != n; ++i)
                if (!is_dead(lam->tvar(i))) analyze(app->arg(n, i));

            for (auto d : lam->deps())
                analyze(d);
            return;
        }
    } else if (auto [lam, var] = def->isa_binder<Lam>(); isa_optimizable(lam)) {
        log().d("unknown lam: {}", lam); // reached as a value, so its signature must stay untouched
        for (auto v : lam->tvars())
            pin(v);
    }

    for (auto d : def->deps())
        analyze(d);
}

/*
 * Transformation
 */

Sieve ADCE::sieve(Lam* lam) {
    if (auto i = lam2sieve_.find(lam); i != lam2sieve_.end()) return i->second;

    auto n = lam->num_tvars();
    if (!isa_optimizable(lam) || !lam->has_var()) return lam2sieve_.emplace(lam, Sieve(n)).first->second;

    auto keep = Sieve(lam->tvars(), [this](const Def* var) { return !analysis_.is_dead(var); });
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
    new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
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
            return map(old_app, new_world().app(new_lam, rewrite(keep.gather(old_app->targs()))));
        }

    return Super::rewrite_imm_App(old_app);
}

const Def* ADCE::rewrite_mut_Lam(Lam* old_lam) {
    if (!sieve(old_lam).all()) return build_lam(old_lam);
    return Super::rewrite_mut_Lam(old_lam);
}

} // namespace mim
