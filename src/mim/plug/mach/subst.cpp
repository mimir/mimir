#include "mim/plug/mach/subst.h"

namespace mim::plug::mach {

const Def* Subst::rewrite(const Def* def) {
    if (target_.is_mem(def)) return m_;
    if (auto new_def = fe::lookup(map_, def)) return new_def;
    if (auto lam = def->isa_mut<Lam>(); lam && lams_)
        if (auto new_lam = fe::lookup(*lams_, lam)) return new_lam;
    if (def->isa_mut() || def->is_closed()) return def;
    if (auto app = def->isa<App>(); app && target_.prev_mem(app))
        def->blame("{} backend: instruction `{}` is not available here", target_.name(), def).bail();
    return Rewriter::rewrite(def);
}

} // namespace mim::plug::mach
