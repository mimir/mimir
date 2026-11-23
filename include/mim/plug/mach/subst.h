#pragma once

#include <mim/rewrite.h>
#include <mim/world.h>

#include "mim/plug/mach/target.h"

namespace mim::plug::mach {

/// Rewrites a value of a block under reconstruction: memory becomes @p m; @p map and @p lams replace values and blocks.
class Subst : public Rewriter {
public:
    Subst(const Target& target, World& world, const Def2Def& map, const Def* m, const LamMap<Lam*>* lams = nullptr)
        : Rewriter(world)
        , target_(target)
        , map_(map)
        , lams_(lams)
        , m_(m) {}

    const Def* rewrite(const Def* def) final;

    /// Rewrites @p term per argument, so an instruction passed as the whole argument gets the new memory.
    const Def* rewrite_jump(const App* term) {
        auto n    = term->num_args();
        auto args = DefVec(n, [&](size_t i) { return rewrite(term->arg(n, i)); });
        return world().app(rewrite(term->callee()), world().tuple(args));
    }

private:
    const Target& target_;
    const Def2Def& map_;
    const LamMap<Lam*>* lams_;
    const Def* m_;
};

} // namespace mim::plug::mach
