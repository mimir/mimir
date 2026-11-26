#pragma once

#include <mim/phase/ds2cps.h>

namespace mim::plug::mach {

/// Turns each `math` operation without a native instruction into a call of its libm function, e.g. `expf`.
/// It runs before `mem.add_mem`, which threads the memory through the new calls.
class Libm : public DS2CPS {
public:
    using DS2CPS::DS2CPS;

private:
    const Def* rewrite_imm_App(const App*) final;

    fe::SymMap<Lam*> decls_;
};

} // namespace mim::plug::mach
