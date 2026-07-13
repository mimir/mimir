#pragma once

#include <mim/phase.h>

namespace mim::plug::regex {

/// Lowers a regex axm application to a DFA matcher function.
class LowerRegex : public RWPhase {
public:
    LowerRegex(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::regex
