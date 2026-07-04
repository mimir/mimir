#include "mim/phase/tail_rec_elim.h"

#include "mim/nest.h"

namespace mim {

bool TailRecElim::is_tail_rec(Lam* lam) {
    if (auto i = tail_rec_.find(lam); i != tail_rec_.end()) return i->second;

    auto ret_var = lam->ret_var();
    if (!ret_var || !lam->is_set() || !lam->is_closed()) return tail_rec_[lam] = false;

    // Scan the (immutable) op graphs of all muts in lam's nest for a call `lam (..., lam's ret var)`.
    auto nest = Nest(lam);
    DefSet done;
    auto found = false;
    auto visit = [&](auto&& visit, const Def* def) -> void {
        if (found || def->isa_mut() || !done.emplace(def).second) return;
        if (auto app = def->isa<App>(); app && app->callee() == lam && app->args().back() == ret_var) {
            found = true;
            return;
        }
        for (auto op : def->deps())
            visit(visit, op);
    };
    for (auto mut : nest.muts())
        if (mut->is_set())
            for (auto op : mut->deps())
                visit(visit, op);

    return tail_rec_[lam] = found;
}

const Def* TailRecElim::rewrite_mut_Lam(Lam* old) {
    if (!is_bootstrapping() && is_tail_rec(old)) {
        auto& w   = new_world();
        auto rec  = w.mut_lam(rewrite(old->type())->as<Pi>())->set(old->dbg());
        auto n    = rec->num_doms();
        auto loop = rec->stub(w.cn(rec->doms().view().rsubspan(1)));
        DLOG("old {} -> (rec: {}, loop: {})", old, rec, loop);
        old2rec_loop_[old] = {rec, loop};
        map(old, rec);

        // The body lives in loop; its vars replace old's vars - except the ret var, which stays rec's.
        DefVec loop_args(n - 1), loop_vars(n);
        for (size_t i = 0; i != n - 1; ++i) {
            loop_args[i] = rec->var(n, i);
            loop_vars[i] = loop->var(n - 1, i);
        }
        loop_vars.back() = rec->var(n, n - 1);
        map(old->var(), w.tuple(loop_vars));

        loop->set(rewrite(old->filter()), rewrite(old->body()));
        rec->app(false, loop, loop_args);
        return rec;
    }

    return RWPhase::rewrite_mut_Lam(old);
}

const Def* TailRecElim::rewrite_imm_App(const App* app) {
    if (auto old = app->callee()->isa_mut<Lam>(); old && !is_bootstrapping() && is_tail_rec(old)) {
        rewrite(old); // make sure rec/loop exist
        auto [rec, loop] = old2rec_loop_[old];
        auto new_args    = DefVec(app->args().size(), [&](size_t i) { return rewrite(app->arg(app->num_args(), i)); });
        if (new_args.back() == rec->vars().back()) return new_world().app(loop, new_args.view().rsubspan(1));
        return new_world().app(rec, new_args);
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim
