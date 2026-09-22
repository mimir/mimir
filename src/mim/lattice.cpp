#include "mim/lattice.h"

#include <algorithm>

#include <fe/algo.h>

#include "mim/check.h"
#include "mim/lam.h"

#include "mim/util/gid.h"

namespace mim {

size_t Bound::find(const Def* type) const {
    assert(isa_imm() && "TODO: doesn't work for mutables");
    auto lt = GIDLt<const Def*>();
    auto i  = isa_mut() ? std::find(ops().begin(), ops().end(), type) : fe::binary_find(ops(), type, lt);
    return i == ops().end() ? size_t(-1) : i - ops().begin();
}

DefVec Match::cases(const Def* scrutinee) {
    auto type = scrutinee->unfold_type();
    if (!type) return {};
    if (auto join = type->isa<Join>()) return DefVec(join->ops().begin(), join->ops().end());
    if (auto variant = type->isa<Variant>()) return DefVec(variant->ops().begin(), variant->ops().end());
    return DefVec{type};
}

bool Match::accepts(const Def* arm, const Def* c, bool infer) {
    auto alpha = [infer](const Def* a, const Def* b) {
        return infer ? Checker::alpha<Checker::Check>(a, b) : Checker::alpha<Checker::Test>(a, b);
    };

    auto pi = arm->isa_type<Pi>();
    if (!pi) return false;
    if (alpha(pi->dom(), c)) return true;
    if (auto join = pi->dom()->isa_imm<Join>())
        return std::ranges::any_of(join->ops(), [&](const Def* op) { return alpha(op, c); });
    return false;
}

} // namespace mim
