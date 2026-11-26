#include "mim/phase/sccp.h"

namespace mim {

const Def* SCCP::Analysis::propagate(Lam* lam, const Def* var, const Def* def) {
    // `⊥ ⊔ x` is `x`, but unusable if lam nests it.
    if (lam->nests(def)) return pin(var), var;

    auto cur = lattice(var);
    if (!cur) { // ⊥ ⊔ def = def; lattice(var, def) invalidates, as it inserts a fresh non-⊤ fact
        lattice(var, def);
        log().d("propagate {} → {}", var, def);
        return def;
    }

    if (cur == var) return cur; // ⊤ stays ⊤

    // cur ⊔ ⊥ = cur, but the ⊥-site's caller must see cur as well.
    if (def->isa<Bot>()) {
        if (auto caller = curr_mut(); caller && !cur->in_scope(caller)) return pin(var), var;
        return cur;
    }

    if (cur == def) return cur; // cur ⊔ cur = cur

    if (cur->isa<Bot>()) { // ⊥ ⊔ def = def; lattice(var, def) invalidates, as it overwrites cur
        lattice(var, def);
        return def;
    }

    return pin(var), var; // two different values join to ⊤; lattice(var, var) therein invalidates, as it overwrites cur
}

const Def* SCCP::Analysis::rewrite_imm_App(const App* app) {
    if (auto res = rewrite_known(app)) return res;
    return mim::Analysis::rewrite_imm_App(app);
}

DefVec SCCP::Analysis::join_args(Lam* lam, DefVec& abstr_targs) {
    return DefVec(abstr_targs.size(), [&](size_t i) { return propagate(lam, lam->tvar(i), abstr_targs[i]); });
}

} // namespace mim
