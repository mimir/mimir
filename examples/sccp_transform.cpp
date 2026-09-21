#include "sccp.h"

namespace mim {

const Def* SCCP::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>()) {
        if (auto l = lattice(old_lam->var()); l && l != old_lam->var()) {
            invalidate();

            auto old_vars = old_lam->tvars();
            auto keep     = Sieve(old_vars, [this](const Def* var) { return is_top(var); });

            Lam* new_lam;
            if (auto i = lam2lam_.find(old_lam); i != lam2lam_.end())
                new_lam = i->second;
            else {
                auto new_doms     = rewrite(keep.gather(old_lam->doms(keep.num_old())));
                new_lam           = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
                lam2lam_[old_lam] = new_lam;

                auto new_vars = DefVec(keep.num_old(), [&](size_t i) {
                    auto j = keep[i];
                    return j == Sieve::Gone ? rewrite(lattice(old_vars[i])) // SCCP propagate
                                            : new_lam->var(keep.num_new(), j);
                });

                map(old_lam->var(), new_vars);
                new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
            }

            return map(old_app, new_world().app(new_lam, rewrite(keep.gather(old_app->targs()))));
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim
