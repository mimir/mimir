#include "mim/plug/core/phase/lower_dyn_extract.h"

#include <algorithm>

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

const Extract* LowerDynExtract::isa_dyn(const Def* type) {
    if (auto ex = type->isa<Extract>();
        ex && ex->type()->isa<Type>() && ex->tuple()->isa<Tuple>() && !ex->index()->isa<Lit>())
        return ex;
    return nullptr;
}

const Def* LowerDynExtract::cast(const Def* type, const Def* val) {
    auto& w = new_world();
    if (val->type() == type) return val;
    if (auto inhabitant = w.inhabitant(type)) return inhabitant;
    if (w.inhabitant(val->type())) return w.bot(type);
    return w.call<bitcast>(type, val);
}

const Def* LowerDynExtract::rewrite_imm_Extract(const Extract* ex) {
    auto& w = new_world();

    if (auto dyn = isa_dyn(ex)) {
        auto data = DefVec();
        for (auto t : dyn->tuple()->ops()) {
            auto new_t = rewrite(t);
            if (!w.inhabitant(new_t) && !std::ranges::contains(data, new_t)) data.emplace_back(new_t);
        }
        if (data.empty()) return w.sigma();
        return data.size() == 1 ? data.front() : w.call<Union>(w.tuple(data));
    }

    if (auto dyn = isa_dyn(ex->type()); dyn && dyn->index() == ex->index()) {
        auto u     = rewrite(dyn);
        auto tuple = rewrite(ex->tuple());
        auto n     = dyn->tuple()->num_ops();
        auto ops   = DefVec(n, [&](size_t i) { return cast(u, tuple->proj(n, i)); });
        return w.extract(w.tuple(ops), rewrite(ex->index()));
    }

    return RWPhase::rewrite_imm_Extract(ex);
}

const Def* LowerDynExtract::rewrite_imm_Insert(const Insert* ins) {
    auto sigma = ins->tuple()->unfold_type()->isa_imm<Sigma>();
    if (!sigma || ins->index()->isa<Lit>() || !isa_dyn(ins->value()->type())) return RWPhase::rewrite_imm_Insert(ins);

    auto& w    = new_world();
    auto tuple = rewrite(ins->tuple());
    auto val   = rewrite(ins->value());
    auto n     = sigma->num_ops();
    auto ops = DefVec(n, [&](size_t i) { return w.insert(tuple, w.lit_idx(n, i), cast(rewrite(sigma->op(i)), val)); });
    return w.extract(w.tuple(ops), rewrite(ins->index()));
}

const Def* LowerDynExtract::coerce_leaf(const Def* new_type, const Def* new_val) {
    if (Axm::isa<Union>(new_type) || Axm::isa<Union>(new_val->type())) return cast(new_type, new_val);
    return nullptr;
}

} // namespace mim::plug::core::phase
