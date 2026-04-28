#include "sccp.h"

namespace mim {

const Def* SCCP::Analysis::propagate(const Def* var, const Def* def) {
    auto [i, ins] = lattice_.emplace(var, def);
    if (ins) {
        invalidate();
        DLOG("propagate: {} → {}", var, def);
        return def;
    }

    auto cur = i->second;
    if (!cur || def->isa<Bot>() || cur == def || cur == var) return cur;

    invalidate();
    if (cur->isa<Bot>()) return i->second = def;
    return i->second = var; // top
}

const Def* SCCP::Analysis::rewrite_imm_App(const App* app) {
    if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);
        auto abstr_vars = absl::FixedArray<const Def*>(n);

        // propagate
        for (size_t i = 0; i != n; ++i) {
            auto abstr    = rewrite(app->targ(i));
            abstr_vars[i] = propagate(lam->tvar(i), abstr);
            abstr_args[i] = abstr;
        }

        // set new abstract var
        auto abstr_var = world().tuple(abstr_vars);
        map(lam->var(), abstr_var);
        lattice_[lam->var()] = abstr_var;

        if (!lookup(lam))
            for (auto d : lam->deps())
                rewrite(d);

        return world().app(lam, abstr_args);
    }

    return mim::Analysis::rewrite_imm_App(app);
}

} // namespace mim
