#include "mim/pass/tail_rec_elim.h"

#include "mim/pass/eta_red.h"

namespace mim {

void TailRecElim::init(PassMan* man) {
    Pass::init(man);
    eta_red_ = man->find<EtaRed>();
}

const Def* TailRecElim::rewrite(const Def* def) {
    if (auto [app, old] = isa_apped_mut_lam(def); old) {
        if (auto i = old2rec_loop_.find(old); i != old2rec_loop_.end()) {
            auto [rec, loop] = i->second;
            auto args        = app->args();
            if (auto ret_var = rec->ret_var(); app->args().back() == ret_var)
                return world().app(loop, args.view().rsubspan(1));
            else
                return world().app(rec, args);
        }
    }

    return def;
}

undo_t TailRecElim::analyze(const Def* def) {
    if (auto [app, old] = isa_apped_mut_lam(def); old) {
        if (auto ret_var = old->ret_var(); ret_var && app->args().back() == ret_var) {
            if (auto [i, ins] = old2rec_loop_.try_emplace(old, std::pair<Lam*, Lam*>(nullptr, nullptr)); ins) {
                auto& [rec, loop] = i->second;
                rec               = old->stub(old->type());
                auto doms         = rec->doms();
                auto loop_dom     = doms.view().rsubspan(1);
                loop              = rec->stub(world().cn(loop_dom));
                DLOG("old {} -> (rec: {}, loop: {})", old, rec, loop);

                auto n = rec->num_doms();
                DefVec loop_args(n - 1);
                DefVec loop_vars(n);
                for (size_t i = 0; i != n - 1; ++i) {
                    loop_args[i] = rec->var(i);
                    loop_vars[i] = loop->var(i);
                }
                loop_vars.back() = rec->vars().back();

                loop->set(old->reduce(world().tuple(loop_vars)));
                rec->app(false, loop, loop_args);
                if (eta_red_) eta_red_->mark_irreducible(loop);

                return undo_visit(old);
            }
        }
    }

    return No_Undo;
}

} // namespace mim
