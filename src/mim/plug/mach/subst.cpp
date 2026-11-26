#include "mim/plug/mach/subst.h"

namespace mim::plug::mach {

const Def* Subst::rewrite(const Def* def) {
    if (target_.is_mem(def)) return m_;
    if (auto new_def = fe::lookup(map_, def)) return new_def;
    if (auto lam = def->isa_mut<Lam>(); lam && lams_)
        if (auto new_lam = fe::lookup(*lams_, lam)) return new_lam;
    // The rebuilt Lam appends φs to its Var, so a use of the whole old Var - e.g. after η-reduction - takes its prefix.
    if (auto var = def->isa<Var>(); var && lams_)
        if (auto lam = var->binder()->isa_mut<Lam>())
            if (auto new_lam = fe::lookup(*lams_, lam)) {
                auto n = lam->num_vars(), new_n = new_lam->num_vars();
                return world().tuple(DefVec(n, [&](size_t i) -> const Def* {
                    auto p = lam->var(n, i);
                    if (target_.is_mem(p)) return m_;
                    if (auto new_p = fe::lookup(map_, p); new_p && p != var) return new_p;
                    return new_lam->var(new_n, i);
                }));
            }
    if (def->isa_mut() || def->is_closed()) return def;
    if (auto app = def->isa<App>(); app && target_.prev_mem(app))
        def->blame("{} backend: instruction `{}` is not available here", target_.name(), def).bail();
    return Rewriter::rewrite(def);
}

} // namespace mim::plug::mach
