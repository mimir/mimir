#pragma once

#include "mim/pass.h"

namespace mim {

class EtaExp;

/// Perform Scalarization (= Argument simplification).
/// This means that, e.g.,
/// ```
/// f := λ (x_1:[T_1, T_2], .., x_n:T_n).E
/// ```
/// will be transformed to
/// ```
/// f' := λ (y_1:T_1, y_2:T_2, .. y_n:T_n).E[x_1 \ (y_1, y_2); ..; x_n \ y_n]
/// ```
/// if `f` appears in callee position only (see @p EtaExp).
/// It will not flatten mutable @p Sigma%s or @p Arr%ays.
class Scalarize : public RWPass<Scalarize, Lam> {
public:
    Scalarize(World& world, flags_t annex)
        : RWPass(world, annex) {}

    void init(PassMan*) final;

    const Def* rewrite(const Def*) override;

private:
    bool should_expand(Lam* lam);
    Lam* make_scalar(const Def* def);

    EtaExp* eta_exp_;
    Lam2Lam tup2sca_;
};

} // namespace mim
