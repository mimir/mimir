#include "mim/target.h"

#include "mim/world.h"

namespace mim {

size_t Target::num_allocatable(Class cls) const {
    size_t n = 0;
    for (size_t r = 0, e = num_regs(cls); r != e; ++r)
        n += !reserved(cls, r);
    return n;
}

size_t Target::num_caller_saved(Class cls) const {
    size_t n = 0;
    for (size_t r = 0, e = num_regs(cls); r != e; ++r)
        n += !reserved(cls, r) && caller_saved(cls, r);
    return n;
}

const Def* Target::mem_def(const Def* def) const {
    if (is_mem(def)) return def;
    if (def->isa<Arr>() || def->type()->isa<Arr>()) return nullptr; // don't look into possibly gigantic arrays
    if (def->num_projs() > 1)
        for (auto proj : def->projs())
            if (auto m = mem_def(proj)) return m;
    return nullptr;
}

DefVec Target::operands(const App* insn) const {
    auto ops = is_axm(annexes_.sched, insn) ? insn->arg()->projs() : insn->args();
    return DefVec(ops.begin(), ops.end());
}

const Def* Target::result(const App* insn, size_t j) {
    auto res = insn->proj(2, 1);
    return res->proj(res->num_projs(), j);
}

const App* Target::rebuild(const App* insn, const Def* m, const DefVec& ops) const {
    auto& w = insn->world();
    if (is_axm(annexes_.sched, insn))
        return w.app(w.app(insn->callee()->as<App>()->callee(), m), w.tuple(ops))->as<App>();
    auto args = ops;
    args[0]   = m;
    return w.app(insn->callee(), w.tuple(args))->as<App>();
}

} // namespace mim
