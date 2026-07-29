#pragma once

#include "mim/phase.h"

namespace mim {

class PrefixCleanup : public RWPhase {
public:
    PrefixCleanup(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    void apply(std::string);
    void apply(const App* app) final { apply(tuple2str(app->arg())); }
    void apply(Phase& p) final { apply(std::move(static_cast<PrefixCleanup&>(p).prefix_)); }

private:
    void rewrite_external(Def*) final;

    std::string prefix_;
};

} // namespace mim
