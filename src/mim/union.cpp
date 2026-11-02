#include "mim/union.h"

#include <algorithm>

#include "mim/check.h"
#include "mim/lam.h"
#include "mim/single.h"

namespace mim {

std::optional<size_t> Join::find_case(const Def* value, bool infer) const {
    auto alpha   = [infer](const Def* a, const Def* b) { return infer ? Check::alpha(a, b) : Check::test_alpha(a, b); };
    auto value_t = value->unfold_type();
    for (size_t i = 0, e = num_ops(); i != e; ++i)
        if (alpha(value_t, op(i))) return i;
    for (size_t i = 0, e = num_ops(); i != e; ++i)
        if (auto single = op(i)->isa<Single>())
            // An open value may still turn out to be the singleton's inhabitant.
            if (alpha(single->op(), value) || (!value->is_closed() && alpha(single->op()->unfold_type(), value_t)))
                return i;
    return {};
}

DefVec Match::cases(const Def* scrutinee) {
    auto type = scrutinee->unfold_type();
    if (!type) return {};
    if (auto join = type->isa<Join>()) return DefVec(join->ops().begin(), join->ops().end());
    if (auto variant = type->isa<Variant>()) return DefVec(variant->ops().begin(), variant->ops().end());
    return DefVec{type};
}

bool Match::accepts(const Def* arm, const Def* c, bool infer) {
    auto alpha = [infer](const Def* a, const Def* b) { return infer ? Check::alpha(a, b) : Check::test_alpha(a, b); };

    auto pi = arm->isa_type<Pi>();
    if (!pi) return false;
    if (alpha(pi->dom(), c)) return true;
    if (auto join = pi->dom()->isa_imm<Join>())
        return std::ranges::any_of(join->ops(), [&](const Def* op) { return alpha(op, c); });
    return false;
}

} // namespace mim
