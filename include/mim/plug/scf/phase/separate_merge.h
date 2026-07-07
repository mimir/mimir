#pragma once

#include <mim/phase.h>

namespace mim::plug::scf::phase {

class MergeSeparator : public mim::NestPhase<Lam> {
public:
    MergeSeparator(World& world, flags_t annex)
        : NestPhase(world, annex, true) {}

    void visit(const Nest& nest) override;
};

} // namespace mim::plug::scf::phase
