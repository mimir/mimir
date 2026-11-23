#include "mim/plug/x86/target.h"

#include <mim/plug/math/math.h>

#include "mim/plug/x86/insn.h"

namespace mim::plug::x86 {

namespace {

/// The `pe` of a `math.F pe`.
const Def* format_of(const Def* type) { return Axm::isa<math::F>(type)->arg(); }

} // namespace

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

Constraint Target::constraint(const App* insn) const {
    auto res = mim::Target::constraint(insn);
    if (Axm::isa<rm>(insn)) {
        res.tied = insn::tied(insn);
        return res;
    }
    if (!Axm::isa<mem::sched>(insn)) return res;

    auto axm = std::get<0>(Axm::get(Target::sched_insn(insn)));
    if (!axm) return res;
    auto id = axm->base();
    if (id == Annex::Base<div> || id == Annex::Base<idiv>) {
        res.fixed.emplace_back(0, encoding(reg::rax));
        res.results.emplace_back(0, encoding(reg::rax));
        res.results.emplace_back(1, encoding(reg::rdx));
        res.clobbers.emplace_back(encoding(reg::rdx));
    } else if (id == Annex::Base<shl> || id == Annex::Base<shr> || id == Annex::Base<sar>) {
        res.fixed.emplace_back(1, encoding(reg::rcx));
    }
    res.tied = insn::tied(id);
    std::erase_if(res.fixed, [&](auto fixed) { return Lit::isa(res.ops[fixed.first]); });
    if (res.tied && Lit::isa(res.ops[*res.tied])) res.tied.reset();
    return res;
}

bool Target::is_removable(const App* insn) const {
    return mim::Target::is_removable(insn) || Axm::isa<load>(insn) || Axm::isa<fload>(insn) || Axm::isa<rm>(insn)
        || Axm::isa<rm1>(insn) || Axm::isa<x86::slot>(insn);
}

const Def* Target::spill(Class cls, const Def* type) const {
    auto& w = type->world();
    return cls == Fpr ? w.app(w.annex<fspill>(), format_of(type)) : w.app(w.annex<x86::spill>(), Idx::as(type));
}

const Def* Target::reload(Class cls, const Def* type) const {
    auto& w = type->world();
    return cls == Fpr ? w.app(w.annex<freload>(), format_of(type)) : w.app(w.annex<x86::reload>(), Idx::as(type));
}

const Def* Target::spill_type(Class cls, const Def* type) const {
    auto& w = type->world();
    return cls == Fpr ? w.app(w.annex<FSpill>(), format_of(type)) : w.app(w.annex<Spill>(), Idx::as(type));
}

const Target& target() {
    static auto res = Target();
    return res;
}

} // namespace mim::plug::x86
