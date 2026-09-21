#include "mim/plug/clos/phase/lower_typed_clos.h"

#include <mim/plug/core/core.h>

namespace mim::plug::clos::phase {

namespace {
const Def* insert_ret(const Def* def, const Def* ret) {
    auto new_ops = DefVec(def->num_projs() + 1, [&](auto i) { return (i == def->num_projs()) ? ret : def->proj(i); });
    auto& w      = def->world();
    return def->is_intro() ? w.tuple(new_ops) : w.sigma(new_ops);
}
} // namespace

void LowerTypedClos::rewrite_external(Def* old_mut) {
    auto new_def = rewrite_root(old_mut);
    // Converted Lam%s are externalized inside make_stub; other externals carry over here.
    if (auto new_mut = new_def->isa_mut(); new_mut && old_mut->is_external() && !new_mut->is_external())
        new_mut->externalize();
}

void LowerTypedClos::finalize() {
    while (!worklist_.empty()) {
        auto [lvm, lcm, old_lam, new_lam] = worklist_.front();
        worklist_.pop();
        lvm_ = lvm;
        lcm_ = lcm;
        log().d("enter {} (lvm = {}, lcm = {})", new_lam, lvm_, lcm_);
        if (old_lam->is_set()) new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
    }
}

Lam* LowerTypedClos::make_stub(Lam* lam, Mode mode, bool adjust_bb_type) {
    assert(lam && "make_stub: not a lam");
    if (auto i = lookup(lam); i && i->isa_mut<Lam>()) return i->as_mut<Lam>();

    auto& w      = new_world();
    auto ep      = env_param(lam->type()->as<Pi>());
    auto new_dom = w.sigma(DefVec(lam->num_doms(), [&](auto i) -> const Def* {
        auto new_dom = rewrite(lam->dom(i));
        if (i == ep) {
            if (mode == Unbox) return env_type();
            if (mode == Box) return w.call<mem::Ptr0>(new_dom);
        }
        return new_dom;
    }));
    if (Lam::isa_basicblock(lam) && adjust_bb_type) new_dom = insert_ret(new_dom, dummy_ret()->type());
    auto new_lam = w.mut_lam(w.cn(new_dom))->set(lam->dbg_key());
    log().d("stub {} → {}", lam, new_lam);
    if (lam->is_external()) new_lam->externalize();

    auto lcm = mem::mem_var(new_lam);
    // The environment always lives in slot `ep`; a single-parameter lam has an atomic var (no projection),
    // so use the whole var there. This selection is independent of `mode` -- the mode only governs how the
    // environment is subsequently consumed (loaded, bitcast, or passed through), not where it sits.
    auto env = new_lam->num_vars() < 2 ? new_lam->var() : new_lam->var(ep);
    if (mode == Box) {
        // A mem-free closure still has to unbox its heap-allocated environment via a mem.load; if it has no mem of
        // its own, a throw-away witness is fine here -- if it actually closes over a real mem, that one is recovered
        // from the unboxed environment below instead (see issue #126).
        if (!lcm) lcm = w.bot(w.call<mem::M>(0));
        auto [m, e] = w.call<mem::load>(Defs{lcm, env})->projs<2>();
        lcm         = m->set("mem");
        env         = e->set("closure_env");
    } else if (mode == Unbox) {
        env = w.call<core::bitcast>(rewrite(lam->dom(ep)), env)->set("unboxed_env");
    }
    auto new_args = w.tuple(DefVec(lam->num_doms(), [&](auto i) {
        return (i == ep) ? env : (lam->var(i) == mem::mem_var(lam)) ? lcm : new_lam->var(i);
    }));
    assert(new_args->num_projs() == lam->num_doms());
    assert(lam->num_doms() <= new_lam->num_doms());
    map(lam->var(), new_args);

    // This closure may not have mem as a direct parameter, yet still close over a real one through its captured
    // environment (e.g. a callback typed without mem that nonetheless uses an outer `mem`). Recover it so
    // mem-effectful operations inside this closure's own body (e.g. packing a further nested closure) have a real
    // chain to thread, instead of none (see issue #126).
    auto lvm = mem::mem_var(lam);
    // `ep < num_vars()` guards a `Cn []` (or mem-only) closure: there is no environment slot to recover a
    // mem from, and `lam->var(ep)` would be an out-of-bounds projection reading past the operand array.
    // Mirrors the `num_vars() < 2` guard applied to `new_lam` above.
    if (!lvm && ep < lam->num_vars()) {
        auto old_env = lam->var(ep);
        if (Axm::isa<mem::M>(old_env->type())) {
            lvm = old_env;
            lcm = env;
        } else if (auto sig = old_env->type()->isa<Sigma>()) {
            for (size_t i = 0, e = sig->num_ops(); i != e; ++i)
                if (Axm::isa<mem::M>(sig->op(i))) {
                    lvm = old_env->proj(i);
                    lcm = env->proj(i);
                    break;
                }
        }
    }
    worklist_.emplace(lvm, lcm, lam, new_lam);
    map(lam, new_lam);
    return new_lam;
}

const Def* LowerTypedClos::rewrite(const Def* def) {
    if (is_bootstrapping()) return RWPhase::rewrite(def); // rebuild the annexes verbatim
    if (auto new_def = lookup(def)) return new_def;

    assert((!def->isa<Var>() || !def->as<Var>()->binder()->isa_mut<Lam>()) && "Lam vars should appear in a map!");

    auto& w = new_world();

    // Lower a closure type `[Env: *, Cn [Env, Args..], Env]` to an untyped `(code-ptr, env-ptr)` pair type.
    if (auto ct = isa_clos_type(def)) {
        auto pi = rewrite(ct->op(1))->as<Pi>();
        if (Pi::isa_basicblock(pi)) pi = w.cn(insert_ret(pi->dom(), dummy_ret()->type()));
        auto env_type = rewrite(ct->op(2));
        return map(def, w.sigma({pi, env_type}));
    }

    // Project out of a closure: index 0 is the (erased) env type, 1 the code, 2 the env.
    if (auto proj = def->isa<Extract>(); proj && isa_clos_type(proj->tuple()->type())) {
        auto idx = Lit::isa(proj->index());
        assert(idx && *idx <= 2 && "unknown proj from closure tuple");
        return map(def, *idx == 0 ? env_type() : rewrite(proj->tuple())->proj(*idx - 1));
    }

    // Lower a closure literal to an untyped `(code-ptr, env-ptr)` pair, boxing/unboxing the environment.
    if (auto c = isa_clos_lit(def)) {
        auto new_type = rewrite(def->type());
        auto env      = rewrite(c.env());
        auto mode     = (env->type()->isa<Idx>() || Axm::isa<mem::Ptr>(env->type())) ? Unbox : Box;
        const Def* fn = make_stub(c.fnc_as_lam(), mode, true);
        if (env->type() == w.sigma()) {
            env = w.bot(env_type()); // optimize empty env
        } else if (mode == Box) {
            auto [mem, env_ptr] = mem::op_alloc(env->type(), lcm_)->projs<2>();
            lcm_                = w.call<mem::store>(Defs{mem, env_ptr, env});
            map(lvm_, lcm_);
            env = env_ptr;
        }
        fn  = w.call<core::bitcast>(new_type->op(0), fn);
        env = w.call<core::bitcast>(new_type->op(1), env);
        return map(def, w.tuple({fn, env}));
    }

    if (auto lam = def->isa_mut<Lam>()) return make_stub(lam, No_Env, false);
    if (def->isa_mut()) {
        assert(!isa_clos_type(def));
        return RWPhase::rewrite_mut(const_cast<Def*>(def));
    }
    if (auto var = def->isa<Var>()) return map(def, w.var(rewrite(var->binder())->as_mut()));

    return RWPhase::rewrite(def);
}

// Give first-class BBs their dummy return continuation.
const Def* LowerTypedClos::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    if (auto p = app->callee()->isa<Extract>();
        p && isa_clos_type(p->tuple()->type()) && Pi::isa_basicblock(app->callee_type())) {
        auto new_arg    = insert_ret(rewrite(app->arg()), dummy_ret()); // arg before callee; see the base hook
        auto new_callee = rewrite(app->callee());
        return new_world().app(new_callee, new_arg);
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerTypedClos::rewrite_imm(const Def* def) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm(def);

    // Leaves and axioms need no mem threading; let the base rebuild them.
    switch (def->node()) {
        case Node::Bot:
        case Node::Top:
        case Node::Type:
        case Node::Univ:
        case Node::Nat:
        case Node::Axm: return RWPhase::rewrite_imm(def);
        default: break;
    }

    auto& w       = new_world();
    auto new_type = rewrite(def->type());
    auto lcm      = lcm_;
    auto new_def  = RWPhase::rewrite_imm(def);

    // Boxing an environment advances lcm_ *while* the operands are rewritten, leaving the operands visited before
    // it on the stale token. lvm_ maps to the new token by now, so redoing the node picks it up; everything else
    // is memoized, so nothing is boxed twice.
    if (lcm_ != lcm) new_def = RWPhase::rewrite_imm(def);

    if (new_type == w.call<mem::M>(0)) { // :store
        lcm_ = new_def;
        lvm_ = def;
    } else if (new_type->isa<Sigma>()) { // :alloc, :slot, ...
        for (size_t i = 0, e = new_type->num_ops(); i != e; ++i)
            if (new_type->op(i) == w.call<mem::M>(0)) {
                lcm_ = w.extract(new_def, i);       // new-world mem chain
                lvm_ = old_world().extract(def, i); // old-world marker, compared against old ops
                break;
            }
    }

    return new_def;
}

} // namespace mim::plug::clos::phase
