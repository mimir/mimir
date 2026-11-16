#include "mim/plug/mem/phase/add_mem.h"

#include <fe/worklist.h>

#include "mim/plug/mem/mem.h"

// TODO make parametric in address space

namespace mim::plug::mem::phase {

namespace {

bool is_mem(const Def* def) { return def->type() && isa_mem(def); } // Level has no type

bool has_mem(const Pi* pi) { return has_mem_ty(pi->dom()) || std::ranges::any_of(pi->doms(), has_mem_ty); }

} // namespace

bool AddMem::analyze() {
    auto queue = fe::BFSWorklist<DefSet>();
    auto pins  = fe::BFSWorklist<DefSet>();
    for (auto mut : old_world().externals().muts())
        queue.push(mut);

    while (!queue.empty()) {
        auto def = queue.pop();

        // These continuations receive the current memory.
        if (auto app = def->isa<App>();
            app && app->axm() && !Axm::isa<mem::fresh>(app) && !Axm::isa<mem::slot>(app) && !Axm::isa<mem::mslot>(app))
            for (auto arg : app->arg()->projs())
                if (auto lam = arg->isa_mut<Lam>()) pins.push(lam);

        for (auto d : def->deps())
            queue.push(d);
    }

    while (!pins.empty()) {
        auto def = pins.pop();
        if (auto lam = def->isa_mut<Lam>()) pins_.emplace(lam);
        for (auto d : def->deps())
            pins.push(d);
    }

    return false;
}

const Def* AddMem::rewrite(const Def* old_def) {
    if (pinned_) {
        // The memo holds the mem-extended variant of a type.
        if (auto pi = old_def->isa_imm<Pi>()) return rewrite_imm_Pi(pi);
        if (auto sigma = old_def->isa_imm<Sigma>()) return rewrite_imm_Sigma(sigma);
        if (auto arr = old_def->isa_imm<Arr>()) return rewrite_imm_Seq(arr);
    } else if (curr_mem_) {
        // Hash-consed equal tuples splice different memories.
        if (auto tuple = old_def->isa<Tuple>(); tuple && std::ranges::any_of(tuple->ops(), is_mem))
            return rewrite_imm_Tuple(tuple);
        auto new_def = Rewriter::rewrite(old_def); // may advance curr_mem_
        return is_mem(old_def) && new_def->type() == curr_mem_->type() ? curr_mem_ : new_def;
    }
    return Rewriter::rewrite(old_def);
}

const Def* AddMem::rewrite_imm_Pi(const Pi* pi) {
    auto new_pi = Rewriter::rewrite_imm_Pi(pi)->as<Pi>();
    if (pinned_ || !Pi::isa_cn(pi) || has_mem(pi)) return new_pi;

    auto& w  = new_world();
    auto mem = w.call<mem::M>(0);

    // Prepending shifts the indices a dependent domain refers to (see issue #177).
    if (auto [sigma, old_var] = new_pi->dom()->isa_binder<Sigma>(); sigma) {
        auto n         = sigma->num_ops();
        auto new_sigma = w.mut_sigma(sigma->type(), n + 1);
        new_sigma->set(0, mem);
        for (size_t i = 0; i != n; ++i) {
            auto shift = w.tuple(DefVec(n, [&](size_t j) { return j < i ? new_sigma->var(n + 1, j + 1) : mem; }));
            new_sigma->set(i + 1, VarRewriter(old_var, shift).rewrite(sigma->op(i)));
        }
        return w.cn(new_sigma);
    }

    return w.cn(Def::cat(mem, new_pi->doms()));
}

const Def* AddMem::rewrite_mut_Lam(Lam* old_lam) {
    if (pinned_ || pins_.contains(old_lam)) {
        auto _  = fe::Restore(pinned_, true);
        auto __ = fe::Restore<const Def*>(curr_mem_, nullptr);
        return Rewriter::rewrite_mut_Lam(old_lam);
    }

    auto& w      = new_world();
    auto new_lam = w.mut_lam(rewrite(old_lam->type())->as<Pi>())->set(old_lam->dbg_key());
    map(old_lam, new_lam);

    auto n      = old_lam->num_vars();
    auto new_n  = new_lam->num_vars();
    auto offset = new_lam->num_doms() - old_lam->num_doms();
    for (size_t i = 0; i != n; ++i)
        map(old_lam->var(n, i), new_lam->var(new_n, i + offset)->set(old_lam->var(n, i)->dbg_key()));
    if (n > 1) map(old_lam->var(), w.tuple(DefVec(n, [&](size_t i) { return new_lam->var(new_n, i + offset); })));

    if (!old_lam->is_set()) return new_lam;

    auto _ = fe::Restore(curr_mem_, mem_var(new_lam));
    return new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
}

const Def* AddMem::rewrite_imm_App(const App* app) {
    if (!curr_mem_) return Rewriter::rewrite_imm_App(app);

    auto& w = new_world();
    if (Axm::isa<mem::fresh>(app)) return w.app(rewrite(app->arg(2, 1)), curr_mem_);

    // The memory effects of the argument precede the call.
    auto new_arg    = rewrite(app->arg());
    auto mem        = curr_mem_;
    auto new_callee = rewrite(app->callee());

    if (auto old_pi = app->callee()->type()->isa<Pi>())
        if (auto new_pi = new_callee->type()->isa<Pi>(); new_pi && new_pi->num_doms() == old_pi->num_doms() + 1)
            new_arg = w.tuple(Def::cat(mem, new_arg->projs(old_pi->num_doms())));

    auto new_app = w.app(new_callee, new_arg);
    if (auto m = mem_def(new_app); m && m->type() == mem->type()) curr_mem_ = m;
    return new_app;
}

const Def* AddMem::rewrite_imm_Tuple(const Tuple* tuple) {
    if (!curr_mem_) return Rewriter::rewrite_imm_Tuple(tuple);

    // Values advance the memory that the memory operands splice, and continuations run after both.
    auto rank = [](const Def* op) {
        if (is_mem(op)) return 1;
        return op->type() && Pi::isa_cn(op->type()) ? 2 : 0;
    };

    auto n       = tuple->num_ops();
    auto new_ops = DefVec(n);
    for (int r = 0; r != 3; ++r)
        for (size_t i = 0; i != n; ++i)
            if (rank(tuple->op(i)) == r) new_ops[i] = rewrite(tuple->op(i));
    return new_world().tuple(rewrite(tuple->type()), new_ops);
}

} // namespace mim::plug::mem::phase
