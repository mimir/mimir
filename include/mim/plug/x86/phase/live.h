#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Prints the Liveness of every function; a debugging and testing aid.
class Live : public ClosedMutPhase<Lam> {
public:
    Live(World& world, flags_t annex)
        : ClosedMutPhase(world, annex, true) {}

private:
    void start() final;
    void visit(Lam*) final;

    std::ostream* os_ = nullptr;
};

} // namespace mim::plug::x86::phase
