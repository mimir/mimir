#include "mim/plug/clos/phase/clos_conv.h"

#include <mim/plug/core/core.h>

#include "mim/rewrite.h"

#include "mim/plug/mem/autogen.h"

using namespace std::literals;

namespace mim::plug::clos::phase {

namespace {

bool is_memop_res(const Def* fd) {
    auto proj = fd->isa<Extract>();
    if (!proj) return false;
    auto types = proj->tuple()->type()->ops();
    return std::ranges::any_of(types, [](auto d) { return Axm::isa<mem::M>(d); });
}

/// The free (non-closed, not-nested) Def%s directly reachable from @p nest's root.
DefSet free_defs(const Nest& nest) {
    DefSet bound, free;
    std::queue<const Def*> queue;
    queue.emplace(nest.root()->mut());

    while (!queue.empty()) {
        for (auto op : pop(queue)->deps()) {
            if (op->is_closed()) continue; // nothing free in here
            if (nest.contains(op)) {
                if (auto [_, ins] = bound.emplace(op); ins) queue.emplace(op);
            } else {
                free.emplace(op);
            }
        }
    }

    return free;
}

} // namespace

/*
 * Free variable analysis
 */

void FreeDefAna::classify(Node* node, const Def* fd, bool& spawned_pred, NodeQueue& worklist) {
    if (fd->is_closed()) return; // e.g. `%mem.M 0` reached as an op of a var-dependent type
    assert(!Axm::isa<mem::M>(fd) && "mem tokens must not be free");

    if (auto [var, lam] = isa_var_proj<Lam>(fd); var && lam) {
        if (var != lam->ret_var()) node->add_fvs(fd);
    } else if (auto free_bb = Axm::isa(attr::free_bb, fd)) {
        node->add_fvs(free_bb);
    } else if (auto pred = fd->isa_mut()) {
        // A referenced nested mutable contributes its own free defs (once it is closure-converted).
        if (pred != node->mut) {
            auto [pnode, inserted] = build_node(pred, worklist);
            node->preds.emplace_back(pnode);
            pnode->succs.emplace_back(node);
            spawned_pred |= inserted;
        }
    } else if (fd->isa<App>() && fd->type()->isa<Pi>()) {
        // A partially applied (curried) function value is pure and rebuildable — memory operations return
        // `[%mem.M, …]` sigmas, never a Pi. Descend into its ops so only the underlying value dependencies
        // are captured; capturing it whole would drag its var-dependent function *type* (e.g.
        // `I64 → Idx n` of a partially applied conversion) into the environment.
        for (auto op : fd->ops())
            classify(node, op, spawned_pred, worklist);
    } else if (fd->has_dep(Dep::Var) && !fd->isa<Tuple>() && fd->is_term()) {
        // Note: a Var may still be closed if its type is a mut, so the isa_var_proj case above is not redundant.
        // A var-dependent *type* (e.g. `%mem.Ptr («n; T», 0)` with a runtime extent `n`) is never captured
        // whole: descend into its ops so the underlying value dependencies (`n`) are captured instead. The
        // body rewrite then rebuilds every occurrence of the type from the substituted value, keeping it
        // definitionally equal to the same type rebuilt inside rewritten operands — an opaque type slot
        // would diverge from those rebuilt copies and break assignability.
        node->add_fvs(fd);
    } else if (is_memop_res(fd)) {
        node->add_fvs(fd); // results of memops must not be floated down
    } else {
        for (auto op : fd->ops())
            classify(node, op, spawned_pred, worklist);
    }
}

std::pair<FreeDefAna::Node*, bool> FreeDefAna::build_node(Def* mut, NodeQueue& worklist) {
    auto [p, inserted] = lam2node_.emplace(mut, nullptr);
    if (!inserted) return {p->second.get(), false};
    world().DLOG("FVA: create node: {}", mut);

    p->second         = std::make_unique<Node>(mut);
    auto node         = p->second.get();
    bool spawned_pred = false;
    for (auto fd : free_defs(Nest(mut)))
        classify(node, fd, spawned_pred, worklist);

    // A node with no fresh predecessors is ready to be settled right away.
    if (!spawned_pred) {
        worklist.push(node);
        world().DLOG("FVA: init {}", mut);
    }
    return {node, true};
}

void FreeDefAna::propagate(NodeQueue& worklist) {
    while (!worklist.empty()) {
        auto node = pop(worklist);
        if (is_done(node)) continue;
        auto changed = is_bot(node);
        mark(node);
        for (auto pred : node->preds)
            for (auto pfv : pred->fvs)
                changed |= node->add_fvs(pfv).second;
        if (changed)
            for (auto succ : node->succs)
                worklist.push(succ);
    }
}

const DefSet& FreeDefAna::run(Lam* lam) {
    auto worklist  = NodeQueue();
    auto [node, _] = build_node(lam, worklist);
    if (!is_done(node)) {
        ++cur_pass_;
        propagate(worklist);
    }
    return node->fvs;
}

/*
 * Closure Conversion
 */

void ClosConv::start() {
    // Bootstrapping: rebuild all annexes verbatim - closure conversion is disabled while converting_ is false.
    for (const auto& [flags, e] : old_world().annexes())
        rewrite_annex(flags, e.sym, e.def);

    // Convert everything reachable from the externals, each in its own fresh substitution scope.
    converting_ = true;
    for (auto old_mut : old_world().externals().muts()) {
        push();
        auto new_def = rewrite(old_mut);
        pop();
        // Non-closure externals (e.g. data) carry their external-ness over directly;
        // converted Lam%s are externalized through their wrapper inside make_stub instead.
        if (auto new_mut = new_def->isa_mut(); new_mut && old_mut->is_external() && !new_mut->is_external())
            new_mut->externalize();
    }

    // Rewrite the deferred closure bodies, each in isolation.
    while (!body_worklist_.empty()) {
        auto fn = body_worklist_.front();
        body_worklist_.pop();
        push();
        rewrite_body(closures_.at(fn));
        pop();
    }

    swap(old_world(), new_world());
}

const Def* ClosConv::rewrite_imm_Pi(const Pi* pi) {
    if (converting_ && Pi::isa_cn(pi)) return clos_type_of(pi);
    return RWPhase::rewrite_imm_Pi(pi);
}

const Def* ClosConv::rewrite_mut_Pi(Pi* pi) {
    if (converting_ && Pi::isa_cn(pi)) return clos_type_of(pi);
    return RWPhase::rewrite_mut_Pi(pi);
}

const Def* ClosConv::rewrite_mut_Lam(Lam* old_lam) {
    if (!converting_ || !Lam::isa_cn(old_lam)) return RWPhase::rewrite_mut_Lam(old_lam);

    auto& w      = new_world();
    auto stub    = make_stub(old_lam);
    auto clos_ty = rewrite(old_lam->type());
    // Rewrite the individual free defs, not the (possibly normalized) env tuple:
    // its normal form may reference defs that are not free vars and hence not in the current map.
    auto env     = w.tuple(DefVec(stub.fvs.size(), [&](auto i) { return rewrite(stub.fvs[i]); }));
    auto closure = clos_pack(env, stub.fn, clos_ty);
    DLOG("RW: pack {} ~> {} : {}", old_lam, closure, clos_ty);
    return map(old_lam, closure);
}

const Def* ClosConv::rewrite_imm_App(const App* app) {
    if (!converting_) return RWPhase::rewrite_imm_App(app);

    if (auto a = Axm::isa<attr>(app))
        if (auto handled = rewrite_attr(a)) return handled;

    auto new_callee = rewrite(app->callee());
    auto new_arg    = rewrite(app->arg());
    if (new_callee->type()->isa<Sigma>()) return clos_apply(new_callee, new_arg);
    // A direct continuation call may mix two spellings of the same dependent extent (see clos_respell) —
    // adapt the argument to the callee's domain. Axm ops keep their arguments verbatim.
    auto root = app->callee();
    while (auto a = root->isa<App>()) root = a->callee();
    if (auto pi = Pi::isa_cn(new_callee->type()); pi && !root->isa<Axm>())
        new_arg = clos_respell(pi->dom(), new_arg);
    return new_world().app(new_callee, new_arg);
}

const Def* ClosConv::rewrite_attr(Axm::IsA<attr, App> a) {
    auto& w = new_world();
    switch (a.id()) {
        case attr::returning:
            // A return continuation is *not* closure converted; it stays a plain Cn sharing the enclosing scope.
            // After η-expansion this should be its only occurrence, so mapping it into the current scope suffices.
            if (auto ret_lam = a->arg()->isa_mut<Lam>()) {
                auto new_doms = DefVec(ret_lam->num_doms(), [&](auto i) { return rewrite(ret_lam->dom(i)); });
                auto new_lam  = w.mut_lam(w.cn(new_doms))->set(ret_lam->dbg());
                map(ret_lam, new_lam);
                if (ret_lam->is_set()) new_lam->set(rewrite(ret_lam->filter()), rewrite(ret_lam->body()));
                return new_lam;
            }
            return nullptr;
        case attr::fstclass_bb:
        case attr::free_bb: {
            // A free/first-class basic block captures nothing: it gets an empty environment and its body is
            // rewritten right here, sharing the enclosing scope (same η-conversion remark as above).
            auto bb_lam = a->arg()->isa_mut<Lam>();
            assert(bb_lam && Lam::isa_basicblock(bb_lam));
            auto stub = make_stub({}, bb_lam);
            auto pack = clos_pack(w.tuple(), stub.fn, rewrite(bb_lam->type()));
            map(bb_lam, pack);
            rewrite_body(stub);
            return pack;
        }
        default: return nullptr;
    }
}

const Def* ClosConv::rewrite_imm_Extract(const Extract* ex) {
    // A closure body may still refer to a ret_var of an *enclosing* Lam: return continuations are not
    // closure-converted, and the FVA deliberately excludes ret_vars, so they are never captured into an env.
    // Map such a projection onto the corresponding var of the enclosing Lam's converted stub.
    // This is a known workaround; the principled fix is to capture escaping enclosing BBs/ret_vars in the
    // environment (tracked by issue #117).
    if (converting_)
        if (auto [var, lam] = isa_var_proj<Lam>(ex); var && lam && lam->ret_var() == var) {
            auto new_fn  = make_stub(lam).fn;
            auto new_idx = skip_env(env_param(new_fn->type()->as<Pi>()), Lit::as(var->index()));
            return new_fn->var(new_idx);
        }
    return RWPhase::rewrite_imm_Extract(ex);
}

const Def* ClosConv::rewrite_mut_Global(Global* global) {
    // Globals are rewritten once and shared, in isolation from any surrounding continuation scope.
    if (auto i = glob_muts_.find(global); i != glob_muts_.end()) return i->second;
    push();
    auto new_global = RWPhase::rewrite_mut_Global(global);
    pop();
    return glob_muts_[global] = new_global;
}

const Pi* ClosConv::rewrite_ret_cn(const Pi* pi) {
    assert(Pi::isa_basicblock(pi));
    return new_world().cn(DefVec(pi->num_doms(), [&](auto i) { return rewrite(pi->dom(i)); }));
}

const Def* ClosConv::clos_type_of(const Pi* pi, const Def* env_type, Defs fvs) {
    if (!env_type)
        if (auto i = glob_muts_.find(pi); i != glob_muts_.end()) return i->second;

    auto& w = new_world();
    const Def* ct;

    auto sigma   = pi->dom()->isa_mut<Sigma>();
    auto old_var = sigma ? sigma->has_var() : nullptr;
    // Captured values leak into the dom types iff the pi is not closed (e.g. `%mem.Ptr («n; T», 0)` naming a
    // captured runtime extent `n`). Such a signature must not be spelled with the *creation context's*
    // values — the lifted function would not be closed, and the spelling would diverge from the body's env
    // projections. Spell them as projections of the env slot instead (see below).
    auto env_spelled = env_type && !fvs.empty() && !pi->is_closed();

    // A *dependent* domain (a mut Sigma whose components reference siblings through its Var, e.g. a runtime
    // extent `n: Nat` named by a pointee `%mem.Ptr («n; T», 0)`) cannot be destructured into a dom list and
    // reassembled — that tears the components off their binder. Instead, splice the env slot into a fresh
    // mut Sigma and remap the old Var to the (env-shifted) new components, like AddMem does for the leading
    // mem (see AddMem::rewrite_imm_Pi). The same builder spells captured values referenced by the dom types
    // as projections of the env slot's component (`env_spelled`).
    if (old_var || env_spelled) {
        auto n     = pi->num_doms();
        auto build = [&](const Def* env_slot, bool project_fvs) -> const Pi* {
            auto new_sigma = w.mut_sigma(n + 1);
            size_t ep      = env_param(pi);
            // Ops must be set in increasing order; a component may only refer to *earlier* components, whose
            // (env-shifted) new Vars are then already set — build the substitution per component; padding
            // slots that are not yet set are never extracted. Each component gets its own rewrite scope so
            // var-remapped rewrites do not leak into the shared memoization.
            for (size_t out = 0; out != n + 1; ++out) {
                if (out == ep) {
                    new_sigma->set(out, env_slot);
                    continue;
                }
                auto i = shift_env(ep, out);
                push();
                if (old_var) {
                    auto shift = w.tuple(DefVec(n, [&](size_t j) {
                        return skip_env(ep, j) < out ? new_sigma->var(n + 1, skip_env(ep, j)) : env_slot;
                    }));
                    map(old_var, shift);
                }
                if (project_fvs && out > ep) {
                    auto env_var = new_sigma->var(n + 1, ep);
                    if (fvs.size() == 1)
                        map(fvs.front(), env_var);
                    else
                        for (size_t fi = 0; fi != fvs.size(); ++fi) map(fvs[fi], env_var->proj(fvs.size(), fi));
                }
                auto comp = (i == n - 1 && Pi::isa_returning(pi)) ? rewrite_ret_cn(pi->ret_pi())
                                                                  : rewrite(pi->dom(i));
                pop();
                new_sigma->set(out, comp);
            }
            return w.cn(new_sigma);
        };
        if (env_type) {
            ct = build(env_type, env_spelled);
        } else {
            auto clos = w.mut_sigma(w.type(), 3_u64)->set("Clos");
            clos->set(0_u64, w.type());
            clos->set(1_u64, build(clos->var(0_u64), false));
            clos->set(2_u64, clos->var(0_u64));
            ct = clos;
        }
    } else {
        auto new_doms = DefVec(pi->num_doms(), [&](auto i) {
            return (i == pi->num_doms() - 1 && Pi::isa_returning(pi)) ? rewrite_ret_cn(pi->ret_pi())
                                                                      : rewrite(pi->dom(i));
        });
        ct            = ctype(w, new_doms, env_type);
    }

    if (!env_type) {
        // A non-closed pi (its types reference an enclosing binder) rewrites differently per context —
        // caching it globally would leak one context's binder into another.
        if (pi->is_closed()) glob_muts_.emplace(pi, ct);
        DLOG("C-TYPE: pct {} ~~> {}", pi, ct);
    } else {
        DLOG("C-TYPE: ct {}, env = {} ~~> {}", pi, env_type, ct);
    }
    return ct;
}

ClosConv::Stub ClosConv::make_stub(const DefSet& fvs, Lam* old_lam) {
    auto& w     = new_world();
    auto fv_vec = DefVec(fvs.begin(), fvs.end());
    // Derive the env parameter type from the tuple of *individually rewritten* free defs — exactly how the
    // packing site (ClosConv::rewrite_mut_Lam) builds the env value. For a dependent env sigma (e.g. a dynamic
    // tensor dimension `n` captured alongside `Idx n`-typed values), rewriting the whole old sigma yields a
    // different dependent sigma than re-tupling the rewritten components, so the two would disagree and
    // clos_pack's `env->type() == pi->dom(ep)` assertion would fail. Building both the same way keeps them
    // in lock-step.
    auto env_type    = w.tuple(DefVec(fv_vec.size(), [&](auto i) { return rewrite(fv_vec[i]); }))->type();
    auto new_fn_type = clos_type_of(old_lam->type(), env_type, fv_vec)->as<Pi>();
    auto new_fn      = w.mut_lam(new_fn_type)->set(old_lam->dbg());

    if (!isa_optimizable(old_lam)) {
        // External or imported (unset) Lam%s get an η-wrapper that hides the environment.
        auto ep = env_param(new_fn_type);
        // A dependent domain (see clos_type_of) must drop its env slot binder-aware: rebuild the mut Sigma
        // one slot smaller and remap the Var. No component references the env slot (it was spliced in
        // fresh), so the padding for it — and for the not-yet-set later slots — is never extracted.
        const Def* new_ext_dom;
        if (auto sig = new_fn_type->dom()->isa_mut<Sigma>(); sig && sig->has_var()) {
            auto n   = sig->num_ops();
            auto res = w.mut_sigma(sig->type(), n - 1);
            for (size_t i = 0, k = 0; i != n; ++i) {
                if (i == ep) continue;
                auto shift = w.tuple(DefVec(n, [&](size_t j) {
                    return (j < i && j != ep) ? res->var(n - 1, shift_env(ep, j)) : w.sigma();
                }));
                auto rw    = VarRewriter(sig->has_var(), shift);
                res->set(k++, rw.rewrite(sig->op(i)));
            }
            new_ext_dom = res;
        } else {
            new_ext_dom = clos_remove_env(ep, new_fn_type->dom());
        }
        auto new_ext_type = w.cn(new_ext_dom);
        auto new_ext_lam  = w.mut_lam(new_ext_type)->set(old_lam->dbg());
        DLOG("wrap ext lam: {} -> stub: {}, ext: {}", old_lam, new_fn, new_ext_lam);
        if (old_lam->is_set()) {
            if (old_lam->is_external()) new_ext_lam->externalize();
            auto env = w.tuple(DefVec(fv_vec.size(), [&](auto i) { return rewrite(fv_vec[i]); }));
            new_ext_lam->app(false, new_fn, clos_insert_env(ep, env, new_ext_lam->var()));
            // new_fn's body is rewritten later via the body worklist.
        } else {
            new_ext_lam->unset();
            new_fn->app(false, new_ext_lam, clos_remove_env(ep, new_fn->var()));
        }
    }

    DLOG("STUB {} ~~> {}", old_lam, new_fn);
    auto stub = Stub{old_lam, std::move(fv_vec), new_fn};
    closures_.try_emplace(old_lam, stub);
    closures_.try_emplace(new_fn, stub);
    return stub;
}

ClosConv::Stub ClosConv::make_stub(Lam* old_lam) {
    if (auto i = closures_.find(old_lam); i != closures_.end()) return i->second;
    auto stub = make_stub(fva_.run(old_lam), old_lam);
    body_worklist_.emplace(stub.fn);
    return stub;
}

void ClosConv::rewrite_body(const Stub& stub) {
    auto old_fn = stub.old_fn;
    if (!old_fn->is_set()) return;

    auto& w      = new_world();
    auto new_fn  = stub.fn;
    auto ep      = env_param(new_fn->type()->as<Pi>());
    auto env_val = new_fn->var(ep)->set("closure_env");
    DLOG("rw body: {} [old={}]", new_fn, old_fn);
    if (stub.fvs.size() == 1) {
        map(stub.fvs.front(), env_val);
    } else {
        for (size_t i = 0, e = stub.fvs.size(); i != e; ++i) {
            auto fv  = stub.fvs[i];
            auto sym = w.sym("fv_"s + (fv->sym() ? fv->sym().str() : std::to_string(i)));
            map(fv, env_val->proj(i)->set(sym));
        }
    }

    auto params = w.tuple(DefVec(old_fn->num_doms(), [&](auto i) { return new_fn->var(skip_env(ep, i)); }));
    map(old_fn->var(), params);

    // A captured value's type may reference another captured value (e.g. `%mem.Ptr («n; T», 0)` captured
    // alongside its runtime extent `n`). Its env projection is typed by the env slot — spelled with the
    // *outer* context's values — while every type rebuilt inside this body spells the same extent as an env
    // projection. Re-spell such a projection with a value-level `%core.bitcast` to the body-canonical type,
    // so it stays definitionally equal to rebuilt signatures (precise-to-precise; a no-op in the backend).
    for (auto fv : stub.fvs) {
        auto mapped = lookup(fv);
        auto want   = rewrite(fv->type());
        if (mapped->type() != want) map(fv, w.call<core::bitcast>(want, mapped)->set(mapped->dbg()));
    }

    new_fn->set(rewrite(old_fn->filter()), rewrite(old_fn->body()));
}

} // namespace mim::plug::clos::phase
