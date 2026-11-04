#include "mim/union.h"

#include <algorithm>

#include "mim/check.h"
#include "mim/lam.h"
#include "mim/single.h"

namespace mim {

static bool alpha(bool infer, const Def* a, const Def* b) {
    return infer ? Check::alpha(a, b) : Check::test_alpha(a, b);
}

std::optional<size_t> Join::case_of(const Def* type, bool infer) const {
    for (size_t i = 0, e = num_ops(); i != e; ++i)
        if (alpha(infer, type, op(i))) return i;
    return {};
}

std::optional<size_t> Join::find_case(const Def* value, bool infer) const {
    auto value_t = value->unfold_type();
    if (auto i = case_of(value_t, infer)) return i;
    for (size_t i = 0, e = num_ops(); i != e; ++i)
        if (auto single = op(i)->isa<Single>())
            // An open value may still turn out to be the singleton's inhabitant.
            if (alpha(infer, single->op(), value)
                || (!value->is_closed() && alpha(infer, single->op()->unfold_type(), value_t)))
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
    auto pi = arm->isa_type<Pi>();
    if (!pi) return false;
    if (alpha(infer, pi->dom(), c)) return true;
    if (auto join = pi->dom()->isa_imm<Join>()) return join->case_of(c, infer).has_value();
    return false;
}

} // namespace mim
