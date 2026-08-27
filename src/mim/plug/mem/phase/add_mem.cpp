#include "mim/plug/mem/phase/add_mem.h"

#include <mim/util/util.h>

#include "mim/plug/mem/mem.h"

// TODO make parametric in address space

namespace mim::plug::mem::phase {

bool AddMem::analyze() {
    // Collect the lams whose ABI is pinned: everything (transitively) reachable from an axm-app argument
    // (combiners, affine index idx, initial accumulators of `%btensor.map_reduce_post`, …).
    auto queue  = unique_queue<DefSet>();
    auto pinned = unique_queue<DefSet>();
    for (auto mut : old_world().externals().muts())
        queue.push(mut);

    while (!queue.empty()) {
        auto def = queue.pop();

        // `%mem.fresh`'s return continuation is *not* pinned: it receives the current memory (see
        // rewrite_imm_App below), so its body must be mem-threaded like any other continuation.
        if (auto app = def->isa<App>(); app && app->axm() && !Axm::isa<mem::fresh>(app))
            for (auto arg : app->arg()->projs())
                if (auto lam = arg->isa_mut<Lam>()) pinned.push(lam);

        for (auto d : def->deps())
            queue.push(d);
    }

    while (!pinned.empty()) {
        auto def = pinned.pop();
        if (auto lam = def->isa_mut<Lam>()) preserved_.emplace(lam);
        for (auto d : def->deps())
            pinned.push(d);
    }

    return false; // one prepass suffices
}

const Def* AddMem::rewrite(const Def* old_def) {
    // Type rewriting is MODE-DEPENDENT: outside a pinned ABI every continuation pi gains a leading
    // mem, inside one it must not - but the rewrite memo is shared. Whichever mode first touches a
    // shared type (e.g. a plain 'Cn F32' return pi) would poison the other, hash-order-dependently.
    // While preserving, rebuild immutable types fresh, neither reading nor storing the memo.
    if (preserving_ && !is_bootstrapping()) {
        if (auto pi = old_def->isa_imm<Pi>()) return rewrite_imm_Pi(pi);
        if (auto sigma = old_def->isa_imm<Sigma>()) return rewrite_imm_Sigma(sigma);
        if (auto arr = old_def->isa_imm<Arr>()) return rewrite_imm_Seq(arr);
    }
    if (curr_mem_ && !preserving_ && !is_bootstrapping()) {
        // A tuple with a direct memory operand is context-dependent: the operand splices to the memory
        // current *at this use*. Two ops with hash-consed identical argument tuples (e.g. two `(⊥, val)`
        // buffer fills) must splice *different* memories - so never store or reuse such a tuple via the
        // rewrite memo; rebuild it at every occurrence.
        if (auto tuple = old_def->isa<Tuple>();
            tuple && std::ranges::any_of(tuple->ops(), [](const Def* op) { return isa_mem(op); }))
            return rewrite_imm_Tuple(tuple);
    }
    auto new_def = Rewriter::rewrite(old_def);
    // Rewrite every memory operand to the current memory - after threading the operand's producers, which
    // advances curr_mem_ along the way. Placeholders (`⊥`/`⊤ : %mem.M 0`) are thereby spliced into the chain.
    if (curr_mem_ && !preserving_ && !is_bootstrapping() && !old_def->isa_mut() && isa_mem(old_def)) return curr_mem_;
    return new_def;
}

const Def* AddMem::rewrite_imm_Pi(const Pi* pi) {
    auto new_pi = Rewriter::rewrite_imm_Pi(pi)->as<Pi>();
    if (is_bootstrapping() || preserving_) return new_pi;

    // Only continuations are mem-extended; a pi that already threads memory is left alone.
    if (Pi::isa_cn(pi) && !has_leading_mem(pi)) {
        auto& w  = new_world();
        auto mem = w.call<mem::M>(0);
        auto dom = new_pi->dom();

        // A dependent domain refers to its own Var.
        // So prepending a leading mem shifts every component's index by one.
        // Rebuild the domain as a fresh mutable Sigma and remap the old domain-Var to the shifted components of the new
        // one. Otherwise the dependent references dangle (see issue #177).
        if (auto [sigma, old_var] = dom->isa_binder<Sigma>(); sigma) {
            auto n         = sigma->num_ops();
            auto new_sigma = w.mut_sigma(sigma->type(), n + 1);
            new_sigma->set(0, mem);
            // Component `i` may only refer to earlier components, whose (index-shifted) new Vars are already
            // set - so build the substitution per component; padding slots `≥ i` are never extracted.
            for (size_t i = 0; i != n; ++i) {
                auto shift = w.tuple(DefVec(n, [&](size_t j) { return j < i ? new_sigma->var(n + 1, j + 1) : mem; }));
                auto rw    = VarRewriter(old_var, shift);
                new_sigma->set(i + 1, rw.rewrite(sigma->op(i)));
            }
            return w.cn(new_sigma);
        }

        auto new_dom = DefVec();
        new_dom.emplace_back(mem);
        for (size_t i = 0, e = new_pi->num_doms(); i != e; ++i)
            new_dom.emplace_back(new_pi->dom(i));
        return w.cn(new_dom);
    }
    return new_pi;
}

const Def* AddMem::rewrite_mut_Lam(Lam* old_lam) {
    if (is_bootstrapping() || preserving_) return Rewriter::rewrite_mut_Lam(old_lam);

    // Pinned ABI (an axm-app argument and everything below it): rewrite verbatim - no memory threaded or added.
    if (preserved_.contains(old_lam)) {
        auto _ = Restore(preserving_, true);
        return Rewriter::rewrite_mut_Lam(old_lam);
    }

    auto new_lam = new_world().mut_lam(rewrite(old_lam->type())->as<Pi>())->set(old_lam->dbg_key());
    map(old_lam, new_lam);

    // Map the parameters, accounting for a possibly inserted leading mem var.
    if (auto n = old_lam->num_vars(); n != 0) {
        auto offset = new_lam->num_doms() - old_lam->num_doms(); // 1 iff we prepended a mem var
        for (size_t i = 0; i != n; ++i)
            map(old_lam->var(i), new_lam->var(i + offset)->set(old_lam->var(i)->dbg_key()));
        // A use of the whole parameter tuple is reconstructed from the new (shifted) components.
        if (n > 1)
            map(old_lam->var(), new_world().tuple(DefVec(n, [&](size_t i) { return new_lam->var(i + offset); })));
    }

    if (!old_lam->is_set()) return new_lam;

    // The body's current memory is this lam's (leading or grouped) mem parameter - or none for direct-style fns.
    auto _ = Restore(curr_mem_, mem::mem_var(new_lam));
    new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
    return new_lam;
}

const Def* AddMem::rewrite_imm_App(const App* app) {
    if (is_bootstrapping() || preserving_ || !curr_mem_) return Rewriter::rewrite_imm_App(app);

    auto& w = new_world();

    // `%mem.fresh (a, k)`: the request for a fresh memory resolves to the memory that is current right
    // here - jump to `k` with it. (Like the rest of this phase, only address space 0 is threaded.)
    if (Axm::isa<mem::fresh>(app)) {
        auto [_, k] = app->args<2>();
        return w.app(rewrite(k), curr_mem_);
    }
    // Rewrite the argument before the callee (as the base Rewriter does). This threads the current memory
    // through the argument's memory effects first; and because operands are rewritten before their users, a
    // shared memory operation is anchored in the scope that consumes its result mem (its own scope) rather
    // than one that merely reuses its non-mem result. curr_mem_ then holds the memory *after* the argument.
    auto new_arg    = rewrite(app->arg());
    auto mem        = curr_mem_;
    auto new_callee = rewrite(app->callee());

    auto old_pi = app->callee()->type()->isa<Pi>();
    auto new_pi = new_callee->type()->isa<Pi>();
    if (old_pi && new_pi && new_pi->num_doms() == old_pi->num_doms() + 1) {
        // The callee gained a leading mem parameter: splice the current memory in front of the arguments.
        auto n    = old_pi->num_doms();
        auto args = DefVec(n + 1);
        args[0]   = mem;
        for (size_t i = 0; i != n; ++i)
            args[i + 1] = new_arg->proj(n, i);
        new_arg = w.tuple(args);
    }

    auto new_app = w.app(new_callee, new_arg);
    advance_mem(new_app);
    return new_app;
}

const Def* AddMem::rewrite_imm_Tuple(const Tuple* tuple) {
    if (is_bootstrapping() || preserving_ || !curr_mem_) return Rewriter::rewrite_imm_Tuple(tuple);

    // The current memory must be threaded through the operands in the right order, because a memory operand
    // (which resolves to the *current* memory) is positioned freely relative to the operands that establish
    // the real ordering. Rewrite in three groups:
    //   1. plain values first - e.g. the `buf` of a buffer op's `(⊥ : %mem.M 0, buf)` argument, whose memory
    //      effects must precede the memory operand;
    //   2. memory operands next - now resolving to the up-to-date current memory;
    //   3. continuation values last - their bodies run later, so a shared memory operation they capture must
    //      already have been anchored (threaded) in this scope by the memory operands above.
    auto rank = [](const Def* op) { return isa_mem(op) ? 1 : (op->type() && Pi::isa_cn(op->type()) ? 2 : 0); };

    auto& w      = new_world();
    auto n       = tuple->num_ops();
    auto new_ops = DefVec(n);
    for (int r = 0; r != 3; ++r)
        for (size_t i = 0; i != n; ++i)
            if (rank(tuple->op(i)) == r) new_ops[i] = rewrite(tuple->op(i));
    return w.tuple(rewrite(tuple->type()), new_ops);
}

void AddMem::advance_mem(const Def* def) {
    if (auto m = mem_def(def)) curr_mem_ = m;
}

} // namespace mim::plug::mem::phase
