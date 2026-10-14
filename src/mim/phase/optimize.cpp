#include "mim/phase/optimize.h"

#include "mim/driver.h"
#include "mim/phase.h"

namespace mim {

void optimize(World& world) {
    world.driver().move_to_library(world);

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

    // make all functions `[] -> compile.Phase` internal
    for (auto def : world.externals().mutate()) {
        if (auto lam = def->isa<Lam>(); lam && lam->num_doms() == 0) {
            if (lam->codom()->sym().view() == "compile.Phase") {
                if (!compilation) compilation = lam;
                def->internalize();
            }
        }
    }

    if (!compilation) {
        world.log().i("no compilation function found; skipping optimization");
    } else {
        world.log().d("compile with {}: {}", compilation, compilation->type());

        world.log().d("build pipeline");
        if (auto phase = Phase::create(world.driver().phases(), compilation->as<Lam>()->body())) phase->run();
    }
}

} // namespace mim
