#include "mim/plug/arm/target.h"

namespace mim::plug::arm {

Target::Target()
    : mim::Target("arm",
                  Annex::Base<core::M>,
                  Annex::Base<mem::sched>,
                  Annex::Base<arm::slot>,
                  Annex::Base<cc>,
                  Annex::Base<arm::Flags>) {}

Target::Class Target::cls(const Def* def) const {
    auto type = def->type();
    if (Axm::isa<arm::Flags>(type)) return Flags;
    if (Axm::isa<Spill>(type)) return Stack;
    return Gpr;
}

bool Target::is_machine_insn(const App* app) const {
    auto axm = app->axm();
    return axm && axm->plugin() == Plugin_Id && !app->type()->isa<Pi>() && !Axm::isa<arm::in>(app);
}

const Def* Target::in(Class, size_t r, const Def* v) const {
    auto& w = v->world();
    return w.call<arm::in>(w.lit_idx(32, r), v);
}

bool Target::is_removable(const App* insn) const {
    return mim::Target::is_removable(insn) || Axm::isa<load>(insn) || Axm::isa<arm::slot>(insn);
}

const Def* Target::spill(Class, const Def* type) const {
    auto& w = type->world();
    return w.app(w.annex<arm::spill>(), Idx::as(type));
}

const Def* Target::reload(Class, const Def* type) const {
    auto& w = type->world();
    return w.app(w.annex<arm::reload>(), Idx::as(type));
}

const Def* Target::spill_type(Class, const Def* type) const {
    auto& w = type->world();
    return w.app(w.annex<Spill>(), Idx::as(type));
}

const Target& target() {
    static auto res = Target();
    return res;
}

} // namespace mim::plug::arm
