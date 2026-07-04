#pragma once

#include "mim/phase.h"

namespace mim {

/// Perform Scalarization (= Argument simplification).
/// This means that, e.g.,
/// ```
/// f := λ (x_1:[T_1, T_2], .., x_n:T_n).E
/// ```
/// will be transformed to
/// ```
/// f' := λ (y_1:T_1, y_2:T_2, .. y_n:T_n).E[x_1 \ (y_1, y_2); ..; x_n \ y_n]
/// ```
/// if `f` appears in callee position only (see @p EtaExpPhase).
/// It will not flatten mutable @p Sigma%s or @p Arr%ays.
class Scalarize : public RWPhase {
public:
    Scalarize(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Should (and can) @p lam be expanded? Memoized; checks type shape and that lam is used as callee only.
    bool should_expand(Lam* lam);
    /// Type-only part of should_expand().
    bool expandable_type(Lam* lam);
    /// Scans the old world once and fills escaped_ with all Lam%s used other than as (branch) callee.
    void analyze_uses();

    LamMap<bool> decided_;
    LamSet escaped_;
    bool uses_analyzed_ = false;
};

} // namespace mim
