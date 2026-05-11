#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class Foobar : public RWPhase {
public:
    Foobar(World& world)
        : RWPhase(world, "Foobar") {}
    Foobar(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);

    DefSet analyzed_;
};

} // namespace mim
