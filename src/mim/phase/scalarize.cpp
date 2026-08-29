#include "mim/phase/scalarize.h"

#include "mim/lam.h"
#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

/// Number of params a single `u64` keep-bitmask can represent; wider doms fall back to the ⊤ sentinel.
static constexpr auto BitmaskWidth = sizeof(u64) * 8;

/// The only Pi%s we ever reshape: immutable (non-dependent) continuations.
static const Pi* isa_flattenable(const Def* def) {
    if (auto pi = Pi::isa_cn(def); pi && pi->isa_imm()) return pi;
    return nullptr;
}

/*
 * Analysis - optimistic: every immutable Cn is flattenable until proven otherwise.
 */

bool Scalarize::Analysis::kept(const Pi* pi, size_t dom) const {
    auto abstr = lattice(pi);
    if (!abstr) return false;
    // Too wide for the u64 bitmask: keep() pins the whole Pi for dom >= 64, so a param this wide
    // can only survive as a u64 entry if it was never recorded - conservatively treat it as kept.
    if (dom >= BitmaskWidth) return true;
    if (auto mask = Lit::isa<u64>(abstr)) return (*mask >> dom) & 1; // per-param bitmask
    return true;                                                     // ⊤ sentinel: whole Pi pinned
}

void Scalarize::Analysis::keep(const Pi* pi, size_t dom) {
    // Out of the thresholded arity: plan() only iterates [0, num_tdoms), so such a parameter is never a
    // split candidate anyway - nothing to record.
    if (dom >= pi->num_tdoms()) return;
    auto cur = lattice(pi);
    if (cur && !Lit::isa<u64>(cur)) return; // already ⊤ - monotone, do not downgrade
    // Too wide for the u64 bitmask (only with an unusually large scalarize threshold): conservatively pin
    // the whole Pi rather than risk splitting a dynamically-indexed parameter.
    if (dom >= BitmaskWidth) {
        pin(pi);
    } else {
        auto mask = cur ? Lit::as<u64>(cur) : u64(0);
        auto next = mask | (u64(1) << dom);
        if (next == mask) return;
        lattice_force(pi, world().lit_nat(next));
        DLOG("keep: {} #{}", pi, dom);
    }
}

/// Collects @p def%'s immutable subtree into @p set; stops at mutables.
static void collect(DefSet& set, const Def* def) {
    if (!set.emplace(def).second) return;
    if (def->isa_mut()) return;
    for (auto d : def->deps())
        collect(set, d);
}

void Scalarize::Analysis::pin_tree(const Def* def) {
    auto visited = DefSet();
    pin_tree(def, visited);
}

void Scalarize::Analysis::pin_tree(const Def* def, DefSet& visited) {
    if (!visited.emplace(def).second) return;
    if (auto pi = isa_flattenable(def)) pin(pi);
    for (auto d : def->deps())
        pin_tree(d, visited);
}

void Scalarize::Analysis::inspect(const Def* def) {
    // Everything reachable from an annex is interface: normalizers and backends rely on its exact shape.
    if (is_bootstrapping()) {
        if (auto pi = isa_flattenable(def)) pin(pi);
        return;
    }

    if (auto app = def->isa<App>()) {
        // The shapes an Axm's signature *itself* dictates are interface - never reshape them
        // (e.g. `%affine.For`'s body): normalizers and plugin phases match on their exact shape,
        // and rebuilding the App re-derives them from the Axm's generic type.
        // Subtrees merely *substituted* in via earlier (type) arguments impose no shape, though -
        // they rewrite consistently with the rest of the World (e.g. `T` in `%mem.store T`):
        // seed the walk's visited set with them so they stay flattenable.
        if (app->uncurry_callee()->isa<Axm>()) {
            if (auto pi = isa_flattenable(app->callee_type())) pin(pi); // this very application's shape
            auto skips = DefSet();
            for (auto d = def; auto a = d->isa<App>(); d = a->callee())
                collect(skips, a->arg());
            pin_tree(app->callee_type()->dom(), skips); // what the Axm consumes (e.g. `%affine.For`'s body)
            pin_tree(app->type(), skips);               // what the Axm produces (e.g. `%autodiff.ad f`)

            // An Axm taking a *bare* function argument (`%autodiff.ad f`) is higher-order machinery that
            // will inspect and call that function by its own convention - keep the function's shape even
            // where the Axm's signature is fully polymorphic (`{T: *} → T → ...`).
            // A function buried inside a tuple argument (`%mem.store (mem, ptr, f)`) is just data, though.
            if (auto lam = app->arg()->isa_mut<Lam>()) pin_tree(lam->type());
        }

        // Assignability is alpha-equivalence, so an App may connect a dom and an arg whose types are
        // *distinct* defs (e.g. one with a mutable ret-Pi, one immutable). Flattening decides per def,
        // which would tear such an edge apart - pin both sides.
        // Only for immutable Pis: a dependent dom legitimately differs from the instantiated arg type.
        if (auto pi = app->callee_type(); pi->isa_imm() && pi->dom() != app->arg()->type()) {
            pin_tree(pi->dom());
            pin_tree(app->arg()->type());
        }
    }

    if (def->isa<Var>()) return; // a Var references its mut as op; that is not a use of the Lam

    // A value inside a *dependently*-typed aggregate - e.g. a typed closure `(T, code, env)` of type
    // `[T: *, Cn [.., T, ..], T]` - has its type dictated by the Sigma's binder:
    // reshaping it would tear the aggregate's typing apart (and clos machinery rebuilds against it).
    if (auto tuple = def->isa<Tuple>(); tuple && tuple->type()->isa_mut())
        for (auto op : tuple->ops())
            pin_tree(op->type());

    // An interface Lam (external, annex, or unset declaration) keeps its whole signature:
    // pin everything its type mentions (its ret Pi, callback params, a polymorphic Lam's inner Cn, ...) -
    // other code (plugin phases, foreign callers) builds against these shapes.
    // Only the interface's own (top-level) Pi stays flattenable: it may be shared with internal values
    // (Scalarize::rewrite_mut_Lam preserves the interface's top level by hand).
    if (auto lam = def->isa_mut<Lam>(); lam && !isa_optimizable(lam)) {
        auto visited = DefSet();
        visited.emplace(lam->type());
        for (auto d : lam->type()->deps())
            pin_tree(d, visited);
        // A curried interface (e.g. `fun extern f {s: Nat} (ab: ...)`) reduces to its inner Lam%s
        // upon application; their types are the interface's *instantiated* codomains - distinct defs
        // from the Pi-side codomains pinned above (Lam var vs Pi var) - so pin them whole, too.
        if (lam->is_set())
            for (auto inner = lam->body()->isa_mut<Lam>(); inner;) {
                pin_tree(inner->type(), visited);
                inner = inner->is_set() ? inner->body()->isa_mut<Lam>() : nullptr;
            }
    }

    // As long as an interface Lam is merely called that is a local affair (see Scalarize::rewrite_imm_App),
    // but escaping as a *value* it would meet flattened values of the same type - so pin the whole type.
    for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
        auto op  = def->op(i);
        auto lam = op ? op->isa_mut<Lam>() : nullptr;
        if (!lam || isa_optimizable(lam)) continue;
        if (auto pi = isa_flattenable(lam->type()); pi && !(def->isa<App>() && i == 0)) pin(pi);
    }

    // A parameter that is Extract%ed / Insert%ed via a non-constant index must not be split.
    const Def* idx_tuple = nullptr;
    const Def* idx       = nullptr;
    if (auto ex = def->isa<Extract>())
        idx_tuple = ex->tuple(), idx = ex->index();
    else if (auto in = def->isa<Insert>())
        idx_tuple = in->tuple(), idx = in->index();
    if (!idx_tuple || Lit::isa(idx)) return;

    if (auto var = idx_tuple->isa<Var>()) {
        if (auto pi = isa_flattenable(var->binder()->type())) pin(pi); // whole var indexed dynamically
    } else if (auto proj = idx_tuple->isa<Extract>()) {
        if (auto var = proj->tuple()->isa<Var>()) {
            if (auto pi = isa_flattenable(var->binder()->type())) {
                if (auto i = Lit::isa(proj->index()))
                    keep(pi, *i);
                else
                    pin(pi);
            }
        }
    }
}

const Def* Scalarize::Analysis::rewrite(const Def* old) {
    // Visit the subtree *before* inspecting: pin() seeds `pi ↦ pi` into the rewriter map (via pin),
    // which would short-circuit the traversal into `old` and skip its subtree for the rest of the round -
    // during bootstrapping this would hide nested Pis from the blanket annex pin.
    auto res = mim::Analysis::rewrite(old);
    inspect(old);
    return res;
}

fe::Vector<bool> Scalarize::Analysis::plan(const Def* type) const {
    auto mask = fe::Vector<bool>();
    if (auto pi = isa_flattenable(type)) {
        // A *dependent* domain (a mut Sigma whose components reference siblings through its Var, e.g. a
        // runtime extent `n: Nat` named by a pointee `%mem.Ptr («n; T», 0)`) must not be flattened at all:
        // splitting any component shifts the indices the dependent references are bound to.
        if (auto sig = pi->dom()->isa_mut<Sigma>(); sig && sig->has_var()) return mask;
        auto n   = pi->num_tdoms();
        auto any = false;
        mask.assign(n, false);
        for (size_t i = 0; i != n; ++i) {
            // Only split immutable types: a mutable Sigma's element types may reference
            // the Sigma's own var (e.g. a typed closure `[T: *, Cn [.., T, ..], T]`),
            // which splitting would leave unbound.
            auto t = pi->tdom(i);
            if (!kept(pi, i) && t->isa_imm() && t->num_tprojs() > 1) mask[i] = any = true;
        }
        if (!any) mask.clear();
    }
    return mask;
}

/*
 * Scalarize
 */

const Def* Scalarize::rewrite_imm_Pi(const Pi* pi) {
    auto mask = is_bootstrapping() ? fe::Vector<bool>() : analysis_.plan(pi);
    if (mask.empty()) return RWPhase::rewrite_imm_Pi(pi);

    // build the flattened (one level) domain
    auto doms = DefVec();
    for (size_t i = 0, n = pi->num_tdoms(); i != n; ++i) {
        auto t = rewrite(pi->tdom(i));
        if (mask[i]) {
            auto pieces = t->tprojs();
            doms.insert(doms.end(), pieces.begin(), pieces.end());
        } else {
            doms.emplace_back(t);
        }
    }

    auto& w  = new_world();
    auto sca = w.pi(w.sigma(doms), rewrite(pi->codom()), pi->is_implicit());
    DLOG("scalarize {} ~> {}", pi, sca);
    invalidate();
    return sca;
}

const Def* Scalarize::rewrite_mut_Lam(Lam* old) {
    auto mask = is_bootstrapping() ? fe::Vector<bool>() : analysis_.plan(old->type());
    if (mask.empty()) return RWPhase::rewrite_mut_Lam(old);

    if (!isa_optimizable(old)) {
        // An interface Lam's signature is ABI: rebuild its Pi via the generic hook -
        // top-level shape preserved, inner types still flattened - instead of rewrite(),
        // which would hand back the flattened Pi.
        auto pi = RWPhase::rewrite_imm_Pi(old->type())->as<Pi>();
        return rewrite_stub(old, new_world().mut_lam(pi));
    }

    auto& w  = new_world();
    auto sca = w.mut_lam(rewrite(old->type())->as<Pi>())->set(old->dbg_key());
    DLOG("scalarize {} : {} ~> {} : {}", old, old->type(), sca, sca->type());
    map(old, sca);

    // reassemble the old var one level from the fresh scalar vars
    auto n      = old->num_tvars();
    auto params = DefVec();
    params.reserve(n);
    for (size_t i = 0, v = 0; i != n; ++i) {
        auto t = rewrite(old->tvar(i)->type());
        if (mask[i]) {
            auto pieces = DefVec(t->num_tprojs(), [&](size_t) { return sca->var(v++); });
            params.emplace_back(w.tuple(t, pieces));
        } else {
            params.emplace_back(sca->var(v++));
        }
    }
    map(old->var(), w.tuple(params));

    sca->set(rewrite(old->filter()), rewrite(old->body()));
    return sca;
}

DefVec Scalarize::flatten_args(const App* app, const fe::Vector<bool>& mask) {
    auto args = DefVec();
    for (size_t i = 0, n = app->num_targs(); i != n; ++i) {
        auto arg = rewrite(app->targ(i));
        if (i < mask.size() && mask[i]) {
            auto pieces = arg->tprojs();
            args.insert(args.end(), pieces.begin(), pieces.end());
        } else {
            args.emplace_back(arg);
        }
    }
    return args;
}

const Def* Scalarize::rewrite_imm_App(const App* app) {
    if (!is_bootstrapping()) {
        // A direct call to an interface Lam keeps its argument shape - the callee's signature stays put.
        auto lam = app->callee()->isa_mut<Lam>();
        if (!lam || isa_optimizable(lam))
            if (auto mask = analysis_.plan(app->callee_type()); !mask.empty())
                return new_world().app(rewrite(app->callee()), flatten_args(app, mask));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim
