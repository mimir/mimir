#include "mim/rule.h"

#include <utility>

#include "mim/def.h"
#include "mim/rewrite.h"
#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

std::tuple<const Var*, const Def*> tuple_of_dict(World& world, Def2Def& v2v) {
    if (v2v.empty()) return {nullptr, nullptr};
    std::vector<std::pair<const Def*, size_t>> tuple_of_args;
    const Var* var_of_rule;
    size_t tuple_size = 1;
    for (auto [var, val] : v2v) {
        if (auto v = var->isa<Var>()) return {v, val}; // we have found all values already
        if (auto e = var->isa<Extract>()) {
            auto i      = e->index()->as<Lit>()->get();
            var_of_rule = e->tuple()->isa<Var>();
            tuple_size  = e->tuple()->arity()->as<Lit>()->get();
            tuple_of_args.push_back(std::make_pair(val, i));
        }
    }

    std::vector<const Def*> fin(tuple_size, nullptr);
    for (auto val_pos : tuple_of_args)
        fin[val_pos.second] = val_pos.first;
    if (var_of_rule) {
        for (size_t i = 0; i < tuple_size; i++)
            if (fin[i] == nullptr) fin[i] = world.mut_hole(world.mut_hole_type());
        return {var_of_rule, world.tuple(fin)};
    }
    return {nullptr, nullptr};
}

bool Rule::is_in_rule(const Def* expr) {
    // are we inside a rule ?
    for (auto var : expr->free_vars()) {
        if (var->binder()->isa<Rule>()) {
            // var is introduced by a rule: this is inside of a body of a rule
            return true;
        }
    }
    return false;
}

bool Rule::its_a_match(const Def* expr, Def2Def& v2v) const {
    if (is_in_rule(expr)) return false;
    return Rule::its_a_match_(expr, lhs(), v2v);
}

bool Rule::its_a_match_(const Def* exp1, const Def* exp2, Def2Def& already_seen) const {
    // we assume all vars in exp2 are pattern matching meta variables
    // therefore they match everything
    if (exp1 == exp2) return true;
    if (exp2->isa<Var>()) {
        if (already_seen.contains(exp2)) return exp1 == already_seen[exp2];
        if (exp2->as<Var>()->binder()->isa<Rule>()) {
            already_seen[exp2] = exp1;
            if (exp1->type() != nullptr && exp2->type() != nullptr)
                if (!its_a_match_(exp1->type(), exp2->type(), already_seen)) return false;
            return true;
        }
        return exp1 == exp2;
        // we want to have 2 bound variables that are equal
    }
    if (auto e2 = exp2->isa<Extract>()) {
        if (auto v = e2->tuple()->isa<Var>()) {
            // we have a var in a tuple
            if (already_seen.contains(e2)) return exp1 == already_seen[e2];
            if (v->binder()->isa<Rule>()) {
                already_seen[e2] = exp1;
                if (exp1->type() != nullptr && exp2->type() != nullptr)
                    if (!its_a_match_(exp1->type(), exp2->type(), already_seen)) return false;
                return true;
            }
            return exp1 == exp2;
        }
    }

    if (exp1->node() == exp2->node()) {
        if (exp1->type() != nullptr && exp2->type() != nullptr)
            if (!its_a_match_(exp1->type(), exp2->type(), already_seen)) return false;

        // else we need to check for a match in all branches (except if no dependencies, then check equality)
        if (auto l1 = exp1->isa<Lit>(); auto l2 = exp2->isa<Lit>()) return l1->get() == l2->get();
        if (auto a1 = exp1->isa<Axm>(); auto a2 = exp2->isa<Axm>()) return a1->flags() == a2->flags();
        if (exp2->num_ops() == 0) return true;
        if (exp2->num_ops() != exp1->num_ops()) return false;
        for (size_t i = 0; i < exp2->num_ops(); i++)
            if (!its_a_match_(exp1->op(i), exp2->op(i), already_seen)) return false;
        return true;
    }

    if (auto l1 = exp1->isa<Lit>()) {
        // l1 can come from a normalized axm
        // we check if there is an evaluation of exp2 s.t. it is normalized to l1
        if (!its_a_match_(exp1->type(), exp2->type(), already_seen)) return false;
        // we gathered all values we could for meta vars; we try to evaluate exp2 and see if it gives l1
        auto [var, values] = tuple_of_dict(world(), already_seen);
        // wtf happens if we could not gather all meta vars values ?
        if (var) {
            auto rw         = VarRewriter(var, values);
            auto eval_right = rw.rewrite(exp2);
            if (auto l2 = eval_right->isa<Lit>()) {
                // we have obtained a value; we can compare
                return l1->get() == l2->get();
            }
        }
    }
    return false;
}

const Def* Rule::replace(const Def* expr, Def2Def& v2v) const {
    auto [var, meta_values] = tuple_of_dict(world(), v2v);
    assert(var);
    auto rw = VarRewriter(var, meta_values);
    auto g  = rw.rewrite(guard());
    if (g == world().lit_tt()) return rw.rewrite(rhs());
    return expr;
}

} // namespace mim
