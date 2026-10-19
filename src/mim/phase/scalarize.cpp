#include "mim/phase/scalarize.h"

#include "mim/lam.h"
#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

/// The only Pi%s we ever reshape: immutable (non-dependent) continuations.
static const Pi* isa_flattenable(const Def* def) {
    if (auto pi = Pi::isa_cn(def); pi && pi->isa_imm()) return pi;
    return nullptr;
}

/// A dependent Sigma of values only; other mutable Sigma%s may be recursive or pack an existential, like a closure.
static bool is_value_telescope(const Def* def) {
    if (auto [sigma, var] = def->isa_binder<Sigma>(); var)
        return std::ranges::none_of(sigma->ops(), [](const Def* op) { return op->isa<Type>(); });
    return false;
}

/// Is the argument of the Axm application @p app typed by one of the Axm's type variables?
static bool is_polymorphic_arg(const App* app) {
    auto n = 0;
    for (const Def* d = app; auto a = d->isa<App>(); d = a->callee())
        ++n;

    auto pi = app->uncurry_callee()->type()->isa<Pi>();
    for (; pi && --n != 0;)
        pi = pi->codom()->isa<Pi>();
    if (!pi) return false;

    auto dom = pi->dom();
    if (auto ex = dom->isa<Extract>()) dom = ex->tuple();
    return dom->isa<Var>();
}

/*
 * Analysis
 */

bool Scalarize::Analysis::kept(const Pi* pi, size_t dom) const {
    if (is_top(pi)) return true; // whole Pi pinned
    auto mask = fe::lookup(keeps_, pi);
    return mask && mask->test(dom);
}

void Scalarize::Analysis::keep(const Pi* pi, size_t dom) {
    if (dom >= pi->num_tdoms() || is_top(pi)) return;
    auto& mask = keeps_[pi];
    if (mask.test(dom)) return;
    mask.set(dom);
    log().d("keep {}#{}", pi, dom);
    touch();
}

void Scalarize::Analysis::keep_var(const Def* def) {
    if (auto [var, i] = Var::isa_tproj(def); var) {
        if (auto pi = isa_flattenable(var->binder()->type())) {
            if (i)
                keep(pi, *i);
            else
                pin(pi);
        }
    }
}

void Scalarize::Analysis::pin_opaque(DefSet& structural, const Def* type) {
    if (transparent(type)) {
        if (structural.emplace(type).second)
            for (auto op : type->ops())
                pin_opaque(structural, op);
    } else {
        pin_imm(type, isa_flattenable);
    }
}

void Scalarize::Analysis::keep_indexed(const Def* tuple, const Def* idx) {
    // Only the outermost index must be constant: `t#(0₂, i)` still splits.
    if (Idx::isa(idx->unfold_type())) {
        if (Lit::isa(idx)) return;
    } else if (auto r = Lit::isa(idx->unfold_type()->arity()); r && *r != 0 && Lit::isa(idx->proj(*r, 0))) {
        return;
    }

    keep_var(tuple);
}

const Def* Scalarize::Analysis::rewrite_mut_Lam(Lam* lam) {
    // The interface's signature keeps its shape, which the body only meets via coerce().
    if (!lam->is_rewritable()) {
        auto structural = DefSet();
        pin_opaque(structural, lam->type());
    }
    return mim::Analysis::rewrite_mut_Lam(lam);
}

// pin() maps `pi ↦ pi`, which would cut off the traversal into the old Def, so the hooks below inspect afterwards.

const Def* Scalarize::Analysis::rewrite_imm_App(const App* app) {
    auto res = mim::Analysis::rewrite_imm_App(app);
    // `tensor.buf x` nominates `x` itself.
    if (app->axm() && is_polymorphic_arg(app)) keep_var(app->arg());
    return res;
}

const Def* Scalarize::Analysis::rewrite_imm_Extract(const Extract* ex) {
    auto res = mim::Analysis::rewrite_imm_Extract(ex);
    keep_indexed(ex->tuple(), ex->index());
    return res;
}

const Def* Scalarize::Analysis::rewrite_imm_Insert(const Insert* ins) {
    auto res = mim::Analysis::rewrite_imm_Insert(ins);
    keep_indexed(ins->tuple(), ins->index());
    return res;
}

fe::Bitset Scalarize::Analysis::plan(const Def* type) const {
    auto mask = fe::Bitset();
    if (auto pi = isa_flattenable(type)) {
        // Splitting a dependent domain would shift the indices its Var references.
        if (auto sig = pi->dom()->isa_mut<Sigma>(); sig && sig->has_var()) return mask;
        for (size_t i = 0, n = pi->num_tdoms(); i != n; ++i) {
            auto t = pi->tdom(i);
            if (!kept(pi, i) && (t->isa_imm() || is_value_telescope(t)) && t->num_tprojs() > 1) mask.set(i);
        }
    }
    return mask;
}

/*
 * Keep
 */

const Def* Scalarize::Keep::rewrite(const Def* old_def) {
    if (!has_cn(old_def) && !has_intersection(old_def)) return sca_.rewrite(old_def);
    bind_old(old_def->has_var());
    return Rewriter::rewrite(old_def);
}

bool Scalarize::Keep::has_cn(const Def* def) {
    if (auto [i, ins] = has_cn_.emplace(def, false); !ins) return i->second;
    auto res = isa_flattenable(def) != nullptr;
    for (auto d : def->deps())
        res |= has_cn(d);
    return has_cn_[def] = res;
}

/*
 * Scalarize
 */

const Def* Scalarize::rewrite_imm_Pi(const Pi* pi) { return flatten(*this, pi, analysis_.plan(pi)); }

const Def* Scalarize::rewrite_imm_App(const App* app) {
    if (!app->axm() || !app->arg()->type()->isa<Pi>()) return RWPhase::rewrite_imm_App(app);

    auto arg    = rewrite(app->arg());
    auto keep   = Keep(*this);
    auto callee = keep.rewrite(app->callee());
    return new_world().app(callee, coerce(callee->type()->as<Pi>()->dom(), arg));
}

const Def* Scalarize::flatten(Rewriter& rw, const Pi* pi, const fe::Bitset& mask) {
    if (mask.none()) return RWPhase::rewrite_imm_Pi(pi);

    auto& w   = new_world();
    auto n    = pi->num_tdoms();
    auto ts   = DefVec(n, [&](size_t i) { return rw.rewrite(pi->tdom(i)); });
    auto num  = size_t(0);
    auto deps = false;
    for (size_t i = 0; i != n; ++i) {
        num += mask[i] ? ts[i]->num_tprojs() : 1;
        deps |= mask[i] && ts[i]->isa_binder<Sigma>().second;
    }

    const Def* dom;
    if (deps) {
        // A telescope's pieces refer to their predecessors, which now live in the new domain's binder.
        auto sigma = w.mut_sigma(num);
        for (size_t i = 0, v = 0; i != n; ++i) {
            if (!mask[i]) {
                sigma->set(v++, ts[i]);
                continue;
            }
            auto k     = ts[i]->num_tprojs();
            auto parts = DefVec();
            for (size_t j = 0; j != k; ++j, ++v) {
                sigma->set(v, w.component(ts[i], parts, k, j));
                parts.emplace_back(sigma->var(num, v));
            }
        }
        dom = sigma;
    } else {
        auto doms = DefVec();
        for (size_t i = 0; i != n; ++i) {
            if (mask[i]) {
                auto pieces = ts[i]->tprojs();
                doms.insert(doms.end(), pieces.begin(), pieces.end());
            } else {
                doms.emplace_back(ts[i]);
            }
        }
        dom = w.sigma(doms);
    }

    auto sca = w.pi(dom, rw.rewrite(pi->codom()), pi->is_implicit());
    log().d("scalarize {} → {}", pi, sca);
    invalidate();
    return sca;
}

Lam* Scalarize::stub(Lam* old, const fe::Bitset& mask) {
    auto n     = old->num_tvars();
    auto types = DefVec(n);
    // An interface Lam's own Pi is flattened like any other, but what it mentions keeps its shape.
    auto keep = Keep(*this);
    auto& rw  = old->is_rewritable() ? static_cast<Rewriter&>(*this) : keep;
    auto type = mask.any() ? flatten(rw, old->type()->as<Pi>(), mask) : rw.rewrite(old->type());
    if (old->is_set())
        for (size_t i = 0; i != n; ++i)
            if (mask[i]) types[i] = rw.rewrite(old->tvar(i)->type());

    auto& w  = new_world();
    auto sca = w.mut_lam(type->as<Pi>())->set(old->dbg_key());
    map(old, sca);
    if (!old->is_set() || mask.none()) return sca;

    auto num_sca = sca->num_vars();
    auto params  = DefVec();
    params.reserve(n);
    for (size_t i = 0, v = 0; i != n; ++i) {
        if (mask[i]) {
            auto t      = types[i];
            auto pieces = DefVec(t->num_tprojs(), [&](size_t) { return sca->var(num_sca, v++); });
            params.emplace_back(w.tuple(t, pieces));
        } else {
            params.emplace_back(sca->var(num_sca, v++));
        }
    }
    map(old->var(), w.tuple(params));
    return sca;
}

const Def* Scalarize::rewrite_mut_Lam(Lam* old) {
    auto mask = analysis_.plan(old->type());
    if (mask.none() && old->is_rewritable()) return RWPhase::rewrite_mut_Lam(old);
    return rewrite_stub(old, stub(old, mask));
}

} // namespace mim
