#pragma once

#include <mim/phase/ds2cps.h>

namespace mim::plug::cps {

/// Full CPS conversion in one recursive rewrite.
///
/// Each term-level, direct-style function
/// ```
/// f: [a: A] → B
/// ```
/// becomes
/// ```
/// f_cps: Cn [a: A, Cn B]
/// ```
/// and every use of `f` is replaced by `cps.cps2ds_dep (A, λ a: B) f_cps` to remain type-correct.
/// Each call `App (cps.cps2ds_dep (T, U) k) arg` inside a continuation is lifted (see DS2CPS).
class Conv : public DS2CPS {
public:
    using DS2CPS::DS2CPS;

private:
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;

    /// Converts direct-style `f: [a: A] → B` to `f_cps: Cn [a: A, Cn B]` and returns the `cps2ds_dep`-wrapped f_cps.
    const Def* convert(Lam*);
};

} // namespace mim::plug::cps
