#include "mim/plug/x86/constraint.h"

#include "mim/plug/x86/insn.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

DefVec operands(const App* insn) {
    auto ops = Axm::isa<sched>(insn) ? insn->arg()->projs() : insn->args();
    return DefVec(ops.begin(), ops.end());
}

const Def* result(const App* insn, size_t j) {
    auto res = insn->proj(2, 1);
    return res->proj(res->num_projs(), j);
}

Constraint constraint(const App* insn) {
    auto res = Constraint();
    res.ops  = operands(insn);
    if (Axm::isa<rm>(insn)) {
        res.tied = insn::tied(Annex::Base<rm>);
        return res;
    }
    if (!Axm::isa<sched>(insn)) return res;

    auto axm = std::get<0>(Axm::get(sched_insn(insn)));
    if (!axm) return res;
    auto& w = insn->world();
    auto id = axm->base();
    // clang-format off
    if (id == Annex::Base<div> || id == Annex::Base<idiv>) {
        res.fixed.emplace_back(0, w.annex(reg::rax));
        res.results.emplace_back(0, w.annex(reg::rax));
        res.results.emplace_back(1, w.annex(reg::rdx));
        res.clobbers.emplace_back(w.annex(reg::rdx));
    } else if (id == Annex::Base<shl> || id == Annex::Base<shr> || id == Annex::Base<sar>) {
        res.fixed.emplace_back(1, w.annex(reg::rcx));
    }
    // clang-format on
    res.tied = insn::tied(id);
    std::erase_if(res.fixed, [&](auto fixed) { return Lit::isa(res.ops[fixed.first]); });
    if (res.tied && Lit::isa(res.ops[*res.tied])) res.tied.reset();
    return res;
}

const App* rebuild(const App* insn, const Def* m, const DefVec& ops) {
    auto& w = insn->world();
    if (Axm::isa<sched>(insn)) return w.call<sched>(sched_insn(insn), m, w.tuple(ops))->as<App>();
    auto args = ops;
    args[0]   = m;
    return w.app(insn->callee(), w.tuple(args))->as<App>();
}

} // namespace mim::plug::x86
