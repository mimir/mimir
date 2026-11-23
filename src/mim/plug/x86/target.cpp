#include "mim/plug/x86/target.h"

#include <mim/plug/math/math.h>

namespace mim::plug::x86 {

Target::Target()
    : mim::Target("x86",
                  Annex::Base<core::M>,
                  Annex::Base<mem::sched>,
                  Annex::Base<x86::slot>,
                  Annex::Base<cc>,
                  Annex::Base<x86::Flags>) {}

Target::Class Target::cls(const Def* value) const {
    auto type = value->type();
    if (Axm::isa<x86::Flags>(type)) return Flags;
    if (Axm::isa<Spill>(type) || Axm::isa<FSpill>(type)) return Stack;
    return math::isa_f(type) ? Fpr : Gpr;
}

bool Target::is_machine_insn(const App* app) const {
    auto axm = app->axm();
    return axm && axm->plugin() == Plugin_Id && !app->type()->isa<Pi>() && !Axm::isa<Addr>(app->type())
        && !Axm::isa<x86::in>(app) && !Axm::isa<fin>(app);
}

bool Target::is_mem_insn(const App* app) const {
    return Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<x86::slot>(app) || Axm::isa<rm>(app)
        || Axm::isa<mr>(app) || Axm::isa<rm1>(app) || Axm::isa<fload>(app) || Axm::isa<fstore>(app);
}

const Def* Target::in(Class cls, size_t r, const Def* v) const {
    auto& w = v->world();
    return cls == Gpr ? w.call<x86::in>(w.lit_idx(Num_Regs, r), v) : w.call<fin>(w.lit_idx(Num_Xmm, r), v);
}

const Target& target() {
    static auto res = Target();
    return res;
}

const Def* strip(const Def* def) { return target().strip(def); }
bool is_sched_cc(const App* app) { return target().is_sched_cc(app); }
const App* isa_sched_cc(const Def* cond) { return target().isa_sched_cc(cond); }
const Def* prev_mem(const App* app) { return target().prev_mem(app); }

} // namespace mim::plug::x86
