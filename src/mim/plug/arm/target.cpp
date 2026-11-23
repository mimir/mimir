#include "mim/plug/arm/target.h"

namespace mim::plug::arm {

Target::Target()
    : mim::Target("arm",
                  Annex::Base<core::M>,
                  Annex::Base<mem::sched>,
                  Annex::Base<arm::slot>,
                  Annex::Base<cc>,
                  Annex::Base<arm::Flags>) {}

bool Target::is_machine_insn(const App* app) const {
    auto axm = app->axm();
    return axm && axm->plugin() == Plugin_Id && !app->type()->isa<Pi>() && !Axm::isa<arm::in>(app);
}

const Def* Target::in(Class, size_t r, const Def* v) const {
    auto& w = v->world();
    return w.call<arm::in>(w.lit_idx(32, r), v);
}

const Target& target() {
    static auto res = Target();
    return res;
}

} // namespace mim::plug::arm
