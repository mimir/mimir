#include "mim/stage.h"

#include "mim/driver.h"

namespace mim {

Stage::Stage(World& world, flags_t annex)
    : world_(world)
    , annex_(annex)
    , name_(world.annex(annex)->sym()) {}

std::unique_ptr<Stage> Stage::recreate() {
    auto ctor = driver().stage(annex());
    auto ptr  = (*ctor)(world());
    ptr->apply(*this);
    return ptr;
}

} // namespace mim
