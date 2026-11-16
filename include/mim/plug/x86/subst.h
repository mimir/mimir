#pragma once

#include <mim/rewrite.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// Rewrites a value of a block under reconstruction: memory becomes @p m; @p map and @p lams replace values and blocks.
class Subst : public Rewriter {
public:
    Subst(World& world, const Def2Def& map, const Def* m, const LamMap<Lam*>* lams = nullptr)
        : Rewriter(world)
        , map_(map)
        , lams_(lams)
        , m_(m) {}

    const Def* rewrite(const Def* def) final {
        if (is_mem(def)) return m_;
        if (auto new_def = fe::lookup(map_, def)) return new_def;
        if (auto lam = def->isa_mut<Lam>(); lam && lams_)
            if (auto new_lam = fe::lookup(*lams_, lam)) return new_lam;
        if (def->isa_mut() || def->is_closed()) return def;
        if (auto app = def->isa<App>(); app && prev_mem(app))
            def->blame(MIM_X86_BE "instruction `{}` is not available here", def).bail();
        return Rewriter::rewrite(def);
    }

    /// Rewrites @p term per argument, so an instruction passed as the whole argument gets the new memory.
    const Def* rewrite_jump(const App* term) {
        auto n    = term->num_args();
        auto args = DefVec(n, [&](size_t i) { return rewrite(term->arg(n, i)); });
        return world().app(rewrite(term->callee()), world().tuple(args));
    }

private:
    const Def2Def& map_;
    const LamMap<Lam*>* lams_;
    const Def* m_;
};

} // namespace mim::plug::x86
