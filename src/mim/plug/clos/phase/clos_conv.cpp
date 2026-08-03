#include "mim/plug/clos/phase/clos_conv.h"

#include <mim/plug/mem/autogen.h>

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
    assert(!Axm::isa<mem::M>(fd) && "mem tokens must not be free");
    if (fd->is_closed()) return;

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
    } else if (fd->has_dep(Dep::Var) && !fd->isa<Tuple>()) {
        // Note: a Var may still be closed if its type is a mut, so the isa_var_proj case above is not redundant.
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

const Def* ClosConv::clos_type_of(const Pi* pi, const Def* env_type) {
    if (!env_type)
        if (auto i = glob_muts_.find(pi); i != glob_muts_.end()) return i->second;

    auto new_doms = DefVec(pi->num_doms(), [&](auto i) {
        return (i == pi->num_doms() - 1 && Pi::isa_returning(pi)) ? rewrite_ret_cn(pi->ret_pi()) : rewrite(pi->dom(i));
    });
    auto ct       = ctype(new_world(), new_doms, env_type);
    if (!env_type) {
        glob_muts_.emplace(pi, ct);
        DLOG("C-TYPE: pct {} ~~> {}", pi, ct);
    } else {
        DLOG("C-TYPE: ct {}, env = {} ~~> {}", pi, env_type, ct);
    }
    return ct;
}

ClosConv::Stub ClosConv::make_stub(const DefSet& fvs, Lam* old_lam) {
    auto& w          = new_world();
    auto fv_vec      = DefVec(fvs.begin(), fvs.end());
    auto env_type    = rewrite(old_world().tuple(fv_vec)->type());
    auto new_fn_type = clos_type_of(old_lam->type(), env_type)->as<Pi>();
    auto new_fn      = w.mut_lam(new_fn_type)->set(old_lam->dbg());

    if (!isa_optimizable(old_lam)) {
        // External or imported (unset) Lam%s get an η-wrapper that hides the environment.
        auto ep           = env_param(new_fn_type);
        auto new_ext_type = w.cn(clos_remove_env(ep, new_fn_type->dom()));
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
    new_fn->set(rewrite(old_fn->filter()), rewrite(old_fn->body()));
}

} // namespace mim::plug::clos::phase
