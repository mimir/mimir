#include "mim/phase/eta_conv.h"

namespace mim {

const Def* EtaConv::eta_reduce(const Lam* lam) {
    if (auto i = eta_.find(lam); i != eta_.end()) return i->second;

    fe::Vector<std::pair<const Lam*, const Def*>> chain;
    GIDMap<const Lam*, size_t> index;

    for (auto cur = lam;;) {
        if (auto [i, ins] = index.emplace(cur, chain.size()); !ins) {
            for (auto j = i->second, e = chain.size(); j != e; ++j)
                eta_[chain[j].first] = nullptr;
            chain.resize(i->second);
            break;
        }
        auto f = cur->eta_reduce();
        chain.emplace_back(cur, f);
        auto next = f ? f->isa_mut<Lam>() : nullptr;
        if (!next || eta_.contains(next)) break;
        cur = next;
    }

    for (auto [l, f] : chain)
        eta_[l] = f;
    return eta_[lam];
}

bool EtaConv::analyze() {
    for (auto def : world().roots())
        visit(def, Lattice::Known);
    return false; // no fixed-point necessary
}

void EtaConv::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    if (auto app = def->isa<App>()) {
        visit(app->type(), Lattice::Unknown_1);
        visit(app->callee(), Lattice::Known);
        visit(app->arg(), Lattice::Unknown_1);
    } else {
        for (auto d : def->deps())
            visit(d, Lattice::Unknown_1);
    }
}

void EtaConv::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) {
        // Wrapper-transparency: a use of `λx.f x` counts as a use of `f` at the same lattice `l`.
        // Do *not* descend into the wrapper's body (which would classify `f` as Known).
        if (auto f = eta_reduce(lam)) {
            ++wrapper_uses_[lam]; // a wrapper serving several occurrences must be split into one per occurrence
            return visit(f, l);
        }
        join(lam, l);
    }
    analyze(def);
}

const Def* EtaConv::rewrite(const Def* old_def) {
    if (old_def->is_ground()) return old_def;

    if (auto lam = old_def->isa<Lam>()) {
        if (auto f = eta_reduce(lam)) {
            // η-redex `λx.f x`: reduce unless `f` wants to stay expanded.
            if (!keep_wrapper(f)) {
                profile_count("η-reduction");
                log().d("eta-reduce {} → {}", lam, f);
                invalidate();
                return rewrite(f);
            }
            // Keep it - but as a *fresh* expansion wrapper (with the `tt` filter EtaExp uses), so that a
            // pre-existing wrapper's stale filter does not leak downstream and every occurrence gets its own.
            // Unless it already is exactly that: re-creating it would churn out a new identity on every run.
            auto new_f = rewrite_no_eta(f);
            if (new_f == f && is_canonical_wrapper(lam)) return lam;
            return Lam::eta_expand(new_f);
        } else if (eta_expand(lam)) {
            // bare Lam used in an unknown position more than once or in both positions: η-expand.
            profile_count("η-expansion");
            auto eta = Lam::eta_expand(rewrite_no_eta(lam));
            log().d("eta-expand {} → {}", lam, eta);
            invalidate();
            return eta;
        }
    }

    return Rewriter::rewrite(old_def);
}

const Def* EtaConv::rewrite_no_exp(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>())
        if (auto f = eta_reduce(lam); f && !keep_wrapper(f)) {
            log().d("eta-reduce {} → {}", lam, f);
            invalidate();
            return rewrite_no_exp(f);
        }
    return Rewriter::rewrite(old_def);
}

const Def* EtaConv::rewrite_imm_App(const App* app) {
    auto callee = rewrite_no_exp(app->callee());
    return world().app(callee, rewrite(app->arg()));
}

const Def* EtaConv::rewrite_imm_Var(const Var* var) { return world().var(rewrite_no_eta(var->binder())->as_mut()); }

} // namespace mim
