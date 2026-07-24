#include "mim/plug/clos/phase/branch_clos_elim.h"

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos {

namespace {

std::tuple<std::vector<ClosLit>, const Def*> isa_branch(const Def* callee) {
    if (auto closure_proj = callee->isa<Extract>()) {
        auto inner_proj = closure_proj->tuple()->isa<Extract>();
        if (inner_proj && inner_proj->tuple()->isa<Tuple>() && isa_clos_type(inner_proj->type())) {
            auto branches = std::vector<ClosLit>();
            for (auto op : inner_proj->tuple()->ops())
                if (auto c = isa_clos_lit(op))
                    branches.push_back(std::move(c));
                else
                    return {};
            return {branches, inner_proj->index()};
        }
    }
    return {};
}

} // namespace

const Def* BranchClosElim::rewrite_imm_App(const App* app) {
    if (is_bootstrapping() || !Pi::isa_cn(app->callee_type())) return RWPhase::rewrite_imm_App(app);

    auto& w = new_world();
    if (auto [branches, index] = isa_branch(app->callee()); index) {
        DLOG("FLATTEN BRANCH {}", app->callee());
        auto ep           = env_param(branches[0].fnc_type());
        auto new_branches = w.tuple(DefVec(branches.size(), [&](size_t i) -> const Def* {
            auto c = branches[i];
            if (auto it = branch2dropped_.find(c); it != branch2dropped_.end()) return it->second;
            // Specialize the closure's lam with its env inlined.
            // The Pi is computed on the old-world closure type and then rewritten -
            // rewriting the (mutable, existential) closure type itself does not survive the world change.
            // Memoize the stub first: rewriting the lam below revisits this very branch.
            auto dropped_lam   = w.mut_lam(rewrite(clos_type_to_pi(c.type()))->as<Pi>())->set(c.fnc_as_lam()->dbg());
            branch2dropped_[c] = dropped_lam;
            auto clam          = rewrite(c.fnc_as_lam())->as_mut<Lam>();
            dropped_lam->set(clam->reduce(clos_insert_env(ep, rewrite(c.env()), dropped_lam->var())));
            return dropped_lam;
        }));
        return w.app(w.extract(new_branches, rewrite(index)), clos_remove_env(ep, rewrite(app->arg())));
    }

    return RWPhase::rewrite_imm_App(app);
}

}; // namespace mim::plug::clos
