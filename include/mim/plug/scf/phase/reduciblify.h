#pragma once

#include <mim/phase.h>

namespace mim::plug::scf::phase {

class Reduciblifier : public mim::NestPhase<Lam> {
public:
    Reduciblifier(World& world, flags_t annex)
        : NestPhase(world, annex, true) {}

    void visit(const Nest& nest) override;

    static bool is_reducible(const Nest::Node* node);

private:
    void visit_node(const Nest::Node* node);
};

} // namespace mim::plug::scf::phase
