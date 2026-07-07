#pragma once

#include <mim/phase.h>

namespace mim::plug::scf::phase {

class Structurizer : public mim::NestPhase<Lam> {
public:
    Structurizer(World& world, flags_t annex)
        : NestPhase(world, annex, true) {}

    void visit(const Nest& nest) override;

private:
    using ExitLabels = absl::flat_hash_set<u32>;
    ExitLabels visit_node(const Nest::Node* node);
};

} // namespace mim::plug::scf::phase
