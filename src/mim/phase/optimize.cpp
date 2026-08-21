#include "mim/phase/optimize.h"

#include "mim/driver.h"
#include "mim/phase.h"

namespace mim {

void optimize(World& world) {
    // The freshly compiled world may still contain solved-but-unresolved Hole%s, let's try to resolve them before
    // running the optimization pipeline.
    Cleanup(world).run();

    // clang-format off
    auto compilation_functions = {
        world.sym("_compile"),
        world.sym("_default_compile"),
    };
    // clang-format on

    const Def* compilation = nullptr;
    for (auto compilation_function : compilation_functions) {
        if (auto compilation_ = world.externals()[compilation_function]) {
            if (!compilation) compilation = compilation_;
            compilation_->internalize();
        }
    }

    // make all functions `[] -> %compile.Phase` internal
    for (auto def : world.externals().mutate()) {
        if (auto lam = def->isa<Lam>(); lam && lam->num_doms() == 0) {
            if (lam->codom()->sym().view() == "%compile.Phase") {
                if (!compilation) compilation = lam;
                def->internalize();
            }
        }
    }

    if (!compilation) {
        world.ILOG("no compilation function found - skipping optimization");
    } else {
        world.DLOG("compilation using {} : {}", compilation, compilation->type());

        auto body   = compilation->as<Lam>()->body();
        auto callee = App::uncurry_callee(body);

        world.DLOG("Building pipeline");
        if (auto f = world.driver().phase(callee->flags())) {
            auto stage = (*f)(world);
            auto phase = stage.get()->as<Phase>();
            if (auto app = body->isa<App>()) phase->apply(app);
            phase->run();
        } else
            fe::throwf("no phase registered for stage '{}'; is its plugin loaded?", callee);
    }
}

} // namespace mim
