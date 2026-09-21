#pragma once

#include "mim/phase.h"

namespace mim {

class Unload : public Phase {
public:
    Unload(World& world, flags_t annex)
        : Phase(world, annex) {}

    void apply(std::string);
    void apply(const App* app) final { apply(tuple2str(app->arg())); }
    void apply(Phase& p) final { apply(std::move(static_cast<Unload&>(p).plugin_str_)); }

private:
    void start() final;

    std::string plugin_str_;
    plugin_t plugin_;
};

} // namespace mim
