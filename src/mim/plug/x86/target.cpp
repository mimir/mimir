#include "mim/plug/x86/target.h"

#include "mim/plug/x86/insn.h"

namespace mim::plug::x86 {

bool Target::is_mem_insn(const App* app) const {
    return mach::Target::is_mem_insn(app) || Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<rm>(app)
        || Axm::isa<mr>(app) || Axm::isa<rm1>(app) || Axm::isa<fload>(app) || Axm::isa<fstore>(app);
}

Constraint Target::constraint(const App* insn) const {
    auto res = mim::Target::constraint(insn);
    if (Axm::isa<rm>(insn)) {
        res.tied = insn::tied(insn);
        return res;
    }
    if (!Axm::isa<mach::sched>(insn)) return res;

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
    return mach::Target::is_removable(insn) || Axm::isa<load>(insn) || Axm::isa<fload>(insn) || Axm::isa<rm>(insn)
        || Axm::isa<rm1>(insn);
}

} // namespace mim::plug::x86
