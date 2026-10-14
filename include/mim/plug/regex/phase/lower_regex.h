#pragma once

#include <mim/phase.h>

namespace mim::plug::regex {

/// Lowers a regex axm application to a DFA matcher function.
class LowerRegex : public RWPhase {
public:
    using RWPhase::RWPhase;

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::regex
