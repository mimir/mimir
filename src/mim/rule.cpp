#include "mim/rule.h"

#include "mim/rewrite.h"
#include "mim/world.h"

namespace mim {

namespace {

/// Matches a pattern against a term; the pattern's meta variables are the Var of its Rule.
class Matcher {
public:
    Matcher(World& world, const Var* meta)
        : world_(world)
        , meta_(meta) {}

    bool match(const Def* ptrn, const Def* term) {
        ptrn = ptrn->zonk();
        if (ptrn == term) return true;
        if (is_meta(ptrn)) {
            if (auto val = bound(ptrn)) return val == term;
            binds_[ptrn] = term;
            return match(ptrn->type(), term->type());
        }
        // Hash-consing makes a pattern without Var%s match only itself.
        if (ptrn->local_vars().empty()) return false;

        // η: a tuple pattern matches any term of its type by its projections.
        if (auto tuple = ptrn->isa<Tuple>(); tuple && !term->isa<Tuple>()) {
            if (!match(ptrn->type(), term->type())) return false;
            auto n = tuple->num_ops();
            for (size_t i = 0; i != n; ++i)
                if (!match(tuple->op(i), term->proj(n, i))) return false;
            return true;
        }

        // Only the same node can match, and a Var or an Axm only itself.
        if (ptrn->isa_mut() || ptrn->isa<Var>() || ptrn->isa<Axm>() || ptrn->node() != term->node()
            || ptrn->flags() != term->flags() || ptrn->num_ops() != term->num_ops())
            return false;
        if (ptrn->type() && term->type() && !match(ptrn->type(), term->type())) return false;
        for (size_t i = 0, e = ptrn->num_ops(); i != e; ++i)
            if (!match(ptrn->op(i), term->op(i))) return false;
        return true;
    }

    /// The value of the meta variable @p var; a tuple Var may be bound by its projections only.
    const Def* value(const Def* var) {
        if (auto val = bound(var)) return val;
        auto n = Lit::isa(var->arity());
        if (!n || *n < 2) return nullptr;
        auto elems = DefVec(*n);
        for (size_t i = 0; i != *n; ++i)
            if (elems[i] = value(var->proj(*n, i)); !elems[i]) return nullptr;
        return world_.tuple(elems);
    }

private:
    bool is_meta(const Def* def) const {
        if (auto var = def->isa<Var>()) return var == meta_;
        if (auto ex = def->isa<Extract>(); ex && Lit::isa(ex->index())) return is_meta(ex->tuple());
        return false;
    }

    const Def* bound(const Def* meta) {
        if (auto val = fe::lookup(binds_, meta)) return val;
        if (auto ex = meta->isa<Extract>())
            if (auto tuple = bound(ex->tuple())) return binds_[meta] = world_.extract(tuple, ex->index());
        return nullptr;
    }

    World& world_;
    const Var* meta_;
    Def2Def binds_;
};

} // namespace

const Def* Rho::solve(const Def* type) const {
    auto var     = has_var();
    auto matcher = Matcher(world(), var);
    return var && matcher.match(dom(), type->zonk()) ? matcher.value(var) : nullptr;
}

std::optional<std::array<const Def*, 2>> Rho::instantiate(const Def* type) const {
    if (!has_var()) return std::array{dom(), codom()};
    auto val = solve(type);
    if (!val) return {};
    auto ops = reduce(val);
    return std::array{ops[0], ops[1]};
}

const Def* Rule::dom() const {
    if (!type()->isa_mut()) return type()->dom();
    return (*type()->instantiate(lhs()->type()))[0];
}

const Def* Rule::codom() const {
    if (!type()->isa_mut()) return type()->codom();
    return (*type()->instantiate(lhs()->type()))[1];
}

const Def* Rule::fire(const Def* arg) const {
    if (isa_mut() && !is_set()) return nullptr;
    auto& w  = world();
    auto var = has_var();
    if (!var) return lhs() == arg && guard() == w.lit_tt() ? rhs() : nullptr;

    auto matcher = Matcher(w, var);
    if (!matcher.match(lhs(), arg)) return nullptr;
    auto val = matcher.value(var);
    if (!val) return nullptr;

    auto ops = reduce(val);
    return ops[1] == w.lit_tt() ? ops[2] : nullptr;
}

} // namespace mim
