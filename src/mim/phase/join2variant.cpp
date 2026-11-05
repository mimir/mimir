#include "mim/phase/join2variant.h"

#include <algorithm>

#include "mim/check.h"

namespace mim {

const Def* Join2Variant::lift(const Def* to, const Def* from, const Def* val) {
    auto join = to->isa<Join>();
    if (!join || Check::test_alpha(to, from)) return val;

    auto& w = new_world();
    if (auto sub = from->isa<Join>()) {
        auto ops = DefVec{val};
        for (auto c : sub->ops()) {
            auto arm = w.mut_lam(w.pi(rewrite(c), rewrite(to)));
            ops.emplace_back(arm->set(false, lift(to, c, arm->var())));
        }
        return w.match(ops);
    }

    if (auto i = join->case_of(from)) return w.inj(rewrite(to), *i, val);
    fe::throwf("`{}` is not a case of union type `{}`", from, to);
}

bool Join2Variant::retype(const Axm* axm) {
    return mentions(axm->type(), [](const Def* def) { return def->isa<Join>(); });
}

const Def* Join2Variant::rewrite_imm_Join(const Join* join) { return new_world().variant(rewrite(join->ops())); }

const Def* Join2Variant::rewrite_imm_Inj(const Inj* inj) {
    auto join = inj->type()->isa<Join>();
    if (!join) return RWPhase::rewrite_imm_Inj(inj);

    auto value = inj->value();
    if (auto i = join->case_of(value->unfold_type())) return new_world().inj(rewrite(join), *i, rewrite(value));
    // A singleton case admits its inhabitant, whose own type is no case.
    if (auto i = join->find_case(value)) return new_world().inj(rewrite(join), *i, new_world().wrap(rewrite(value)));
    fe::throwf("`{}` is not a case of union type `{}`", value, join);
}

const Def* Join2Variant::rewrite_imm_Match(const Match* match) {
    auto scrutinee = match->scrutinee();
    auto join      = scrutinee->unfold_type()->isa<Join>();
    auto type      = match->type();
    if (!join && !type->isa<Join>()) return RWPhase::rewrite_imm_Match(match);

    auto& w    = new_world();
    auto ops   = DefVec{rewrite(scrutinee)};
    auto cases = Match::cases(scrutinee);
    for (size_t i = 0, e = cases.size(); i != e; ++i) {
        auto c   = cases[i];
        auto arm = join ? *std::ranges::find_if(match->arms(), [c](const Def* arm) { return Match::accepts(arm, c); })
                        : match->arm(i);
        auto pi  = arm->isa_type<Pi>();
        if (Check::test_alpha(pi->dom(), c) && Check::test_alpha(pi->codom(), type)) {
            ops.emplace_back(rewrite(arm));
        } else {
            auto lam = w.mut_lam(w.pi(rewrite(c), rewrite(type)));
            auto res = w.app(rewrite(arm), lift(pi->dom(), c, lam->var()));
            ops.emplace_back(lam->set(false, lift(type, pi->codom(), res)));
        }
    }

    return w.match(ops);
}

const Def* Join2Variant::rewrite_imm_Extract(const Extract* extract) {
    auto join  = extract->type()->isa<Join>();
    auto sigma = extract->tuple()->unfold_type()->isa_imm<Sigma>();
    if (!join || !sigma) return RWPhase::rewrite_imm_Extract(extract);

    auto tuple = rewrite(extract->tuple());
    auto n     = sigma->num_ops();
    auto ops   = DefVec(n, [&](size_t i) { return lift(join, sigma->op(i), tuple->proj(n, i)); });
    return new_world().extract(new_world().tuple(ops), rewrite(extract->index()));
}

} // namespace mim
