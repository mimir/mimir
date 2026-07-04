#include "mim/phase/lam_spec.h"

#include "mim/nest.h"

// TODO This is supposed to recreate what lower2cff did, but we should really consider another strategy and nuke this.

namespace mim {

const Def* LamSpec::rewrite_imm_App(const App* old_app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(old_app);
    if (auto i = old2new_.find(old_app); i != old2new_.end()) return i->second;

    auto old_lam = old_app->callee()->isa_mut<Lam>();
    if (!isa_optimizable(old_lam)) return RWPhase::rewrite_imm_App(old_app);

    // Skip recursion to avoid infinite inlining if not "aggressive_lam_spec".
    // This is a hack - but we want to get rid off this stage anyway.
    Nest nest(old_lam);
    if (!old_world().flags().aggressive_lam_spec && nest.is_recursive()) return RWPhase::rewrite_imm_App(old_app);

    // Specialize the ordinary new-world copy of old_lam; everything below happens in the new world.
    auto lam = rewrite(old_lam)->as_mut<Lam>();

    DefVec new_doms, new_vars, new_args;
    auto skip = lam->ret_var() && lam->is_closed();
    auto doms = lam->doms();

    for (auto dom : doms.view().rsubspan(skip))
        if (!dom->isa<Pi>()) new_doms.emplace_back(dom);

    if (skip) new_doms.emplace_back(doms.back());
    if (new_doms.size() == lam->num_doms()) return RWPhase::rewrite_imm_App(old_app);

    auto& w      = new_world();
    auto new_lam = lam->stub(w.cn(new_doms));

    // Project new_lam's var with the explicit arity new_doms.size():
    // a single remaining sigma dom is flattened by cn(), so new_lam->var(i) would project into that sigma.
    auto num_new = new_doms.size();
    auto num_old = old_app->num_args();

    for (size_t arg_i = 0, var_i = 0, n = num_old - skip; arg_i != n; ++arg_i) {
        auto arg = rewrite(old_app->arg(num_old, arg_i));
        if (lam->dom(arg_i)->isa<Pi>()) {
            new_vars.emplace_back(arg);
        } else {
            new_vars.emplace_back(new_lam->var(num_new, var_i++));
            new_args.emplace_back(arg);
        }
    }

    if (skip) {
        new_vars.emplace_back(new_lam->var(num_new, num_new - 1));
        new_args.emplace_back(rewrite(old_app->arg(num_old, num_old - 1)));
    }

    new_lam->set(lam->reduce(w.tuple(new_vars)));
    DLOG("{} -> {}: {} -> {})", lam, new_lam, lam->dom(), new_lam->dom());

    return old2new_[old_app] = w.app(new_lam, new_args);
}

} // namespace mim
