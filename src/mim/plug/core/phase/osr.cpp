#include "mim/plug/core/phase/osr.h"

namespace mim::plug::core::phase {

enum {
    Proxy_IV, // proxy(var) <- var is optimistically assumed to be an induction variable
};

static const Def* mk_iv(const Def* var) {
    return var->world().proxy(var->type(), {var}, Proxy_IV, true)->set(var->dbg_key());
}

static bool depends(Lam* lam, const Def* def) {
    auto var = lam->has_var();
    return (var && def->has_free_var(var)) || lam->nests(def);
}

/// Can every site in @p lam's scope use @p t, and does it stay the same while @p lam loops?
static bool is_invariant(Lam* lam, const Def* t) {
    return t->in_scope(lam) && !depends(lam, t) && !RWPhase::mentions(t, [](const Def* d) { return d->isa<Proxy>(); });
}

static_assert(Annex::flags2sub(flags_t(nat::mul)) == sub_t(OSR::Op::mul));
static_assert(Annex::flags2sub(flags_t(wrap::mul)) == sub_t(OSR::Op::mul));

/// Matches `x op y` for core.nat and core.wrap.
static std::optional<std::tuple<OSR::Op, const Def*, const Def*>> isa_op(const Def* def) {
    auto match = [](auto app) { return std::tuple_cat(std::tuple{OSR::Op(app.sub())}, app->template args<2>()); };
    if (auto app = Axm::isa<nat>(def); app && app.sub() <= sub_t(OSR::Op::mul)) return match(app);
    if (auto app = Axm::isa<wrap>(def); app && app.sub() <= sub_t(OSR::Op::mul)) return match(app);
    return {};
}

static const Def* mk_op(OSR::Op o, const Def* a, const Def* b) {
    if (a->type()->isa<Nat>()) return op(nat(Annex::base<nat>() | flags_t(o)), a, b);
    return op(wrap(Annex::base<wrap>() | flags_t(o)), a, b);
}

static std::optional<OSR::Step> isa_step(Lam* lam, const Def* base, const Def* abstr) {
    if (abstr == base) return OSR::Step{OSR::Op::add, nullptr};
    if (auto o = isa_op(abstr)) {
        auto [op, x, y] = *o;
        if (op == OSR::Op::mul) return {};
        if (x == base && is_invariant(lam, y)) return OSR::Step{op, y};
        if (op == OSR::Op::add && y == base && is_invariant(lam, x)) return OSR::Step{op, x};
    }
    return {};
}

/*
 * Analysis
 */

void OSR::Analysis::reset() {
    Super::reset();
    visited_.clear();
    first_.clear();
    stepped_.clear();
    app2steps_.clear();
    lam2cands_.clear();
}

bool OSR::Analysis::is_iv(const Def* var) const {
    auto l = lattice(var);
    return l && Proxy::isa<Proxy_IV>(l) && stepped_.contains(var);
}

const Def* OSR::Analysis::join(const App* site, Lam* lam, size_t i, const Def* abstr_arg) {
    auto var = lam->tvar(i);
    if (lam->is_dependent()) return pin(var), var;

    auto cur = lattice(var);
    if (cur == var) return var; // ⊤ is final
    if (abstr_arg->isa<Bot>()) return cur ? cur : var;

    auto iv = mk_iv(var);
    if (auto step = isa_step(lam, cur == iv ? iv : var, abstr_arg)) {
        auto& steps = app2steps_[site];
        steps.resize(lam->num_tvars());
        steps[i] = step;
        if (step->t) stepped_.emplace(var);
        return lattice(var, iv), iv;
    }

    if (depends(lam, abstr_arg)) {
        log().d("no IV: {} ↦ {}", var, abstr_arg);
        return pin(var), var;
    }

    // An Expr restarts on its first touch per round, as an upstream var may have moved on, e.g. from Expr to IV.
    // rewrite_imm_App() taints all callers whenever lam's abstract vars change, so the round re-joins each of them.
    auto [_, first] = first_.emplace(var);
    if (cur == iv || abstr_arg == cur) return cur;
    if ((!cur || first) && abstr_arg->in_scope(lam)) return lattice(var, abstr_arg), abstr_arg;
    return lattice(var, iv), iv;
}

void OSR::Analysis::cand(const Def* x, const Def* c) {
    if (auto [var, i] = Var::isa_tproj(x); var && i)
        if (auto lam = Lam::isa_rewritable(var->binder()); lam && !depends(lam, c)) {
            auto& cands = lam2cands_[lam];
            if (auto cand = Cand{*i, c}; std::ranges::find(cands, cand) == cands.end()) cands.emplace_back(cand);
        }
}

const Def* OSR::Analysis::rewrite_imm_App(const App* app) {
    if (auto o = isa_op(app); o && std::get<0>(*o) == Op::mul) {
        auto [_, a, b] = *o;
        cand(a, b);
        cand(b, a);
    }

    auto abstr_arg    = rewrite(app->arg());
    auto abstr_callee = rewrite(app->callee());

    if (auto lam = Lam::isa_rewritable(abstr_callee)) {
        auto v          = version();
        auto n          = lam->num_tvars();
        auto abstr_args = abstr_arg->projs(n);
        auto abstr_vars = DefVec(n, [&](size_t i) { return join(app, lam, i, abstr_args[i]); });

        if (n > 1) lattice(lam->var(), world().tuple(abstr_vars));
        if (auto mut = curr_mut()) lam2callers_[lam].emplace(mut);
        if (version() != v)
            for (auto caller : lam2callers_[lam])
                taint(caller);
        // raw_app: World::app would β-reduce or partially evaluate lam.
        return world().raw_app(rewrite(app->type()), lam, abstr_arg);
    }

    return Super::rewrite_imm_App(app);
}

void OSR::Analysis::finalize() {
    for (auto def : world().externals().muts())
        escape(def);
}

// A Lam that escapes as a value gets new params only if every one of its concrete uses is a call.
void OSR::Analysis::escape(const Def* def) {
    if (auto [_, ins] = visited_.emplace(def); !ins) return;

    if (auto app = def->isa<App>()) {
        if (auto lam = Lam::isa_rewritable(app->callee())) {
            escape(app->type());
            escape(app->arg());
            for (auto d : lam->deps())
                escape(d);
            return;
        }
    } else if (auto [lam, var] = def->isa_binder<Lam>(); lam && lam->is_rewritable()) {
        log().d("unknown lam: {}", lam);
        pin(var); // otherwise the whole var's tuple resurfaces an earlier round's value
        for (auto v : lam->tvars())
            pin(v);
    }

    for (auto d : def->deps())
        escape(d);
}

/*
 * Transformation
 */

const OSR::Cands& OSR::cands(Lam* lam) {
    auto [i, ins] = lam2cands_.emplace(lam, Cands());
    if (ins)
        if (auto cands = analysis_.cands(lam))
            for (auto cand : *cands)
                if (analysis_.is_iv(lam->tvar(cand.i))) i->second.emplace_back(cand);
    return i->second;
}

Lam* OSR::build_lam(Lam* old_lam) {
    if (auto new_lam = fe::lookup(lam_old2new_.left(), old_lam)) return new_lam;

    invalidate();
    const auto& cs = cands(old_lam);
    auto n         = old_lam->num_tvars();
    auto m         = n + cs.size();
    auto new_doms  = DefVec(m, [&](size_t i) { return rewrite(old_lam->tdom(i < n ? i : cs[i - n].i)); });
    auto new_lam   = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
    log().d("{} → {}: {} reduced", old_lam, new_lam, cs.size());
    profile_count("osr.muls.reduced", cs.size());
    lam_old2new_.insert(old_lam, new_lam);

    auto old_vars = old_lam->tvars();
    map(old_lam->var(), DefVec(n, [&](size_t i) { return new_lam->var(m, i)->set(old_vars[i]->dbg()); }));
    auto _ = enter(old_lam);
    new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
    return new_lam;
}

const Def* OSR::reduced(const Def* x, const Def* c) {
    if (auto [var, i] = Var::isa_tproj(x); var && i)
        if (auto old_lam = Lam::isa_rewritable(var->binder())) {
            auto cs = cands(old_lam);
            if (auto k = std::ranges::find(cs, Cand{*i, c}); k != cs.end()) {
                auto n = old_lam->num_tvars();
                return build_lam(old_lam)->var(n + cs.size(), n + (k - cs.begin()));
            }
        }
    return nullptr;
}

const Def* OSR::rewrite_imm_App(const App* old_app) {
    if (auto o = isa_op(old_app); o && std::get<0>(*o) == Op::mul) {
        auto [_, a, b] = *o;
        if (auto j = reduced(a, b)) return j;
        if (auto j = reduced(b, a)) return j;
    }

    auto old_lam = old_app->callee()->isa_mut<Lam>();
    // The callee may only fold to a rebuilt Lam in the new World, e.g. a branch whose condition becomes constant.
    if (!old_lam)
        if (auto new_lam = rewrite(old_app->callee())->isa_mut<Lam>())
            old_lam = fe::lookup(lam_old2new_.right(), new_lam);

    if (old_lam)
        if (auto cs = cands(old_lam); !cs.empty()) {
            auto new_lam = build_lam(old_lam);
            auto n       = old_lam->num_tvars();
            auto m       = n + cs.size();
            auto targs   = old_app->targs();
            auto steps   = analysis_.steps(old_app);

            auto new_args = DefVec(m, [&](size_t k) -> const Def* {
                if (k < n) return rewrite(targs[k]);
                auto [i, c] = cs[k - n];
                auto new_c  = rewrite(c);
                if (auto step = steps ? (*steps)[i] : std::nullopt) {
                    auto j = new_lam->var(m, k);
                    if (!step->t) return j;
                    return mk_op(step->op, j, mk_op(Op::mul, rewrite(step->t), new_c));
                }
                return mk_op(Op::mul, rewrite(targs[i]), new_c);
            });

            return new_world().app(new_lam, new_args);
        }

    return Super::rewrite_imm_App(old_app);
}

const Def* OSR::rewrite_mut_Lam(Lam* old_lam) {
    if (!cands(old_lam).empty()) return build_lam(old_lam);
    return Super::rewrite_mut_Lam(old_lam);
}

} // namespace mim::plug::core::phase
