#pragma once

#include <mim/phase.h>

namespace mim::plug::omp::phase {

/// Lifts the body of every `omp.par_for` into a closed kernel over an environment of the values it captures and
/// rewrites the loop into an `omp.launch`.
/// The kernel receives the environment through a pointer and the chunk bounds as `I64`, which is the ABI of the
/// runtime wrapper `mim_par_for`.
class Outline : public RWPhase {
public:
    Outline(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;

private:
    std::optional<DefVec> captures(Lam* body);
};

} // namespace mim::plug::omp::phase
