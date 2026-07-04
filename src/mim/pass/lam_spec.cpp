#include "mim/pass/lam_spec.h"

#include "mim/nest.h"

// TODO This is supposed to recreate what lower2cff did, but we should really consider another strategy and nuke this.

namespace mim {

const Def* LamSpec::rewrite(const Def* def) {
    if (auto i = old2new_.find(def); i != old2new_.end()) return i->second;

    auto [app, old_lam] = isa_apped_mut_lam(def);
    if (!isa_optimizable(old_lam)) return def;

    // Skip recursion to avoid infinite inlining if not "aggressive_lam_spec".
    // This is a hack - but we want to get rid off this Pass anyway.
    Nest nest(old_lam);
    if (!world().flags().aggressive_lam_spec && nest.is_recursive()) return def;

    DefVec new_doms, new_vars, new_args;
    auto skip     = old_lam->ret_var() && old_lam->is_closed();
    auto old_doms = old_lam->doms();

    for (auto dom : old_doms.view().rsubspan(skip))
        if (!dom->isa<Pi>()) new_doms.emplace_back(dom);

    if (skip) new_doms.emplace_back(old_lam->doms().back());
    if (new_doms.size() == old_lam->num_doms()) return def;

    auto new_lam = old_lam->stub(world().cn(new_doms));

    // Project new_lam's var with the explicit arity new_doms.size():
    // a single remaining sigma dom is flattened by cn(), so new_lam->var(i) would project into that sigma.
    auto num_new = new_doms.size();

    for (size_t arg_i = 0, var_i = 0, n = app->num_args() - skip; arg_i != n; ++arg_i) {
        auto arg = app->arg(arg_i);
        if (old_lam->dom(arg_i)->isa<Pi>()) {
            new_vars.emplace_back(arg);
        } else {
            new_vars.emplace_back(new_lam->var(num_new, var_i++));
            new_args.emplace_back(arg);
        }
    }

    if (skip) {
        new_vars.emplace_back(new_lam->var(num_new, num_new - 1));
        new_args.emplace_back(app->args().back());
    }

    new_lam->set(old_lam->reduce(world().tuple(new_vars)));
    DLOG("{} -> {}: {} -> {})", old_lam, new_lam, old_lam->dom(), new_lam->dom());

    return old2new_[def] = world().app(new_lam, new_args);
}

} // namespace mim
