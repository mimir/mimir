#include "mim/plug/clos/phase/branch_clos_elim.h"

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

namespace {

std::tuple<fe::Vector<ClosLit>, const Def*> isa_branch(const Def* callee) {
    if (auto closure_proj = callee->isa<Extract>()) {
        auto sw = Switch(closure_proj->tuple());
        if (sw && sw.tuple()->isa<Tuple>() && isa_clos_type(sw.extract()->type())) {
            auto branches = fe::Vector<ClosLit>();
            for (size_t i = 0, n = sw.num_targets(); i != n; ++i)
                if (auto c = isa_clos_lit(sw.target(i)))
                    branches.emplace_back(c);
                else
                    return {};
            return {branches, sw.index()};
        }
    }
    return {};
}

} // namespace

const Def* BranchClosElim::rewrite_imm_App(const App* app) {
    if (!Pi::isa_cn(app->callee_type())) return RWPhase::rewrite_imm_App(app);

    auto& w = new_world();
    if (auto [branches, index] = isa_branch(app->callee()); index) {
        log().d("flatten branch: {}", app->callee());
        auto ep           = env_param(branches[0].fnc_type());
        auto new_branches = w.tuple(DefVec(branches.size(), [&](size_t i) -> const Def* {
            auto c = branches[i];
            if (auto it = branch2dropped_.find(c); it != branch2dropped_.end()) return it->second;
            // Specialize the closure's lam with its env inlined.
            // The Pi is computed on the old-world closure type and then rewritten -
            // rewriting the (mutable, existential) closure type itself does not survive the world change.
            // Memoize the stub first: rewriting the lam below revisits this very branch.
            auto dropped_lam = w.mut_lam(rewrite(clos_type_to_pi(c.type()))->as<Pi>())->set(c.fnc_as_lam()->dbg_key());
            branch2dropped_[c] = dropped_lam;
            auto clam          = rewrite(c.fnc_as_lam())->as_mut<Lam>();
            dropped_lam->set(clam->reduce(clos_insert_env(ep, rewrite(c.env()), dropped_lam->var())));
            return dropped_lam;
        }));
        return w.app(w.extract(new_branches, rewrite(index)), clos_remove_env(ep, rewrite(app->arg())));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::clos::phase
