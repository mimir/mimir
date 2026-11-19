#include "mim/union.h"

#include "mim/check.h"
#include "mim/lam.h"

namespace mim {

DefVec Match::cases(const Def* scrutinee) {
    auto type = scrutinee->unfold_type();
    if (!type) return {};
    if (auto variant = type->isa<Variant>()) return DefVec(variant->ops().begin(), variant->ops().end());
    return DefVec{type};
}

bool Match::accepts(const Def* arm, const Def* c, bool infer) {
    if (auto pi = arm->isa_type<Pi>()) return infer ? Check::alpha(pi->dom(), c) : Check::test_alpha(pi->dom(), c);
    return false;
}

} // namespace mim
