#include "mim/plug/x86/constraint.h"

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
        res.tied = 1;
        return res;
    }
    if (!Axm::isa<sched>(insn)) return res;

    auto f = sched_insn(insn)->isa<App>();
    if (!f || !f->axm()) return res;
    auto& w = insn->world();
    auto id = f->axm()->flags();
    // clang-format off
    if (id == Annex::Base<div> || id == Annex::Base<idiv>) {
        res.fixed.emplace_back(0, w.annex<rax>());
        res.results.emplace_back(0, w.annex<rax>());
        res.results.emplace_back(1, w.annex<rdx>());
        res.clobbers.emplace_back(w.annex<rdx>());
    } else if (id == Annex::Base<shl> || id == Annex::Base<shr> || id == Annex::Base<sar>) {
        res.tied = 0;
        res.fixed.emplace_back(1, w.annex<rcx>());
    } else if (id == Annex::Base<add> || id == Annex::Base<sub> || id == Annex::Base<and_> || id == Annex::Base<or_>
            || id == Annex::Base<xor_> || id == Annex::Base<imul>) {
        res.tied = 0;
    } else if (id == Annex::Base<cmovcc>) {
        res.tied = 1;
    }
    // clang-format on
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
