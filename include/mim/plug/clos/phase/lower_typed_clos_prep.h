#pragma once

#include <mim/phase.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

/// Escape analysis for closures:
/// closure literals get their function wrapped in `%clos.attr.esc` or `%clos.attr.bottom`,
/// depending on whether their environment escapes.
class LowerTypedClosPrep : public RWPhase {
public:
    LowerTypedClosPrep(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// One escape-propagation round over the old world; RWBase::start() iterates until fixpoint.
    bool analyze() final;
    const Def* rewrite_imm_Tuple(const Tuple*) final;

    bool is_esc(const Def* def) {
        if (auto [_, lam] = isa_var_proj<Lam>(def); lam && !lam->is_set()) return true;
        return esc_.contains(def);
    }
    bool set_esc(const Def*);

    DefSet esc_;
};

} // namespace mim::plug::clos::phase
