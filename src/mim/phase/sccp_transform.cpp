#include "mim/phase/sccp.h"

namespace mim {

const Def* SCCP::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>()) {
        if (abstracted(old_lam->var())) {
            invalidate();

            auto old_vars = old_lam->tvars();
            auto keep     = Sieve(old_vars, [this](const Def* var) { return is_top(var); });

            auto new_lam = fe::lookup(lam2lam_, old_lam);
            if (!new_lam) {
                auto new_doms     = rewrite(keep.gather(old_lam->tdoms()));
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

            return new_world().app(new_lam, rewrite(keep.gather(old_app->targs())));
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim
