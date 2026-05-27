#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::cps {

/// Converts direct style function to cps functions.
/// To do so, for each (non-type-level) ds function a corresponding cps function is created:
/// ```
/// f:     [a: A] -> B
/// f_cps: Cn [a: A, Cn B]
/// ```
/// Only the type signature of the function is changed and the body is wrapped in the newly added return continuation.
/// (Technical detail: the arguments are substituted to fit the new function)
///
/// In a second distinct but connected step, the call sites are converted:
/// For a direct style call `f args`, the call to the cps function `cps2ds_dep ... f_cps args` is introduced.
/// The underlying substitution is `f` -> `cps2ds_dep ... f_cps`.
class DS2CPS : public RWPhase {
public:
    DS2CPS(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) override;

private:
    const Def* rewrite_lam(Lam* lam);

    Def2Def rewritten_;
};

} // namespace mim::plug::cps
