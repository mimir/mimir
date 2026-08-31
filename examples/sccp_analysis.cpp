#include "sccp.h"

namespace mim {

/// The Lam the abstract @p var belongs to; @p var is a Var or a Var projection.
static Lam* lam_of(const Def* var) {
    if (auto ex = var->isa<Extract>()) return ex->tuple()->as<Var>()->binder()->as_mut<Lam>();
    return var->as<Var>()->binder()->as_mut<Lam>();
}

const Def* SCCP::Analysis::propagate(const Def* var, const Def* def) {
    // `⊥ ⊔ x` is `x`, but unusable if lam nests it.
    if (lam_of(var)->nests(def)) return pin(var), var;

    auto cur = lattice(var);
    if (!cur) { // ⊥ ⊔ def = def; lattice(var, def) invalidates, as it inserts a fresh non-⊤ fact
        lattice(var, def);
        log().d("propagate: {} → {}", var, def);
        return def;
    }

    if (def->isa<Bot>() || cur == def || cur == var) return cur; // cur ⊔ ⊥ = cur ⊔ cur = cur; ⊤ stays ⊤

    if (cur->isa<Bot>()) { // ⊥ ⊔ def = def; lattice(var, def) invalidates, as it overwrites cur
        lattice(var, def);
        return def;
    }

    return pin(var), var; // two different values join to ⊤; lattice(var, var) therein invalidates, as it overwrites cur
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

        lattice(lam->var(), world().tuple(abstr_vars)); // set new abstract var
        return world().app(rewrite(lam), abstr_args);
    }

    return mim::Analysis::rewrite_imm_App(app);
}

} // namespace mim
