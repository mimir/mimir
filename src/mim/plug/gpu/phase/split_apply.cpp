
#include <mim/driver.h>

#include "mim/plug/gpu/phase/split_apply.h"

namespace mim::plug::gpu::phase {

// run extendable pipelines as a phase inspired by optimize.cpp
static void run_stage(World& world, flags_t annex) {
    const Def* stages = world.annex(annex);
    auto body         = stages->as<Lam>()->body();
    auto callee       = App::uncurry_callee(body);

    auto create_stage = world.driver().stage(callee->flags());
    if (!create_stage) error("Could not get stage");

    auto stage = (*create_stage)(world);
    auto phase = stage.get()->as<Phase>();
    auto app   = body->isa<App>();
    if (!app) error("Stage is unexpectedly not an App");

    phase->apply(app);
    phase->run();
}

void SplitApply::start() {
    split_phase.run();
    swap(old_world(), split_phase.old_world());
    swap(new_world(), split_phase.new_world());

    run_stage(old_world(), Annex::base<gpu::host_specific_phases>());
    run_stage(new_world(), Annex::base<gpu::device_specific_phases>());
}

} // namespace mim::plug::gpu::phase
