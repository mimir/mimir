#include "mim/phase/single_erasure.h"

namespace mim {

static bool is_shaped(const Def* def) { return def->isa<Sigma>() || def->isa<Arr>() || def->isa<Variant>(); }

/*
 * Analysis
 */

const Def* SingleErasure::Analysis::rewrite(const Def* old) {
    // Visit the subtree *before* inspecting: pin() seeds `def ↦ def` into the rewriter map, which would
    // short-circuit the traversal into `old` and skip its subtree for the rest of the round.
    auto res = mim::Analysis::rewrite(old);

    if (auto app = old->isa<App>()) {
        // An Axm's signature dictates arities (`Idx n_groups` becomes `Idx 1` all over again).
        pin_axm(app, is_shaped);

        // Assignability is alpha-equivalence, so an App may connect a dom and an arg whose types are *distinct*
        // defs - a dependent dom and its instance (`[n: Nat, «n; *»]` vs `[Nat, []]`). Dropping decides per def,
        // which would tear such an edge apart: the instance loses what the dependent dom still expects.
        if (auto dom = app->callee_type()->dom(); dom != app->arg()->type()) {
            auto visited = DefSet();
            pin_imm(visited, dom, is_shaped);
            pin_imm(visited, app->arg()->type(), is_shaped);
        }
    }

    return res;
}

/*
 * SingleErasure
 */

bool SingleErasure::is_gone(const Def* type) {
    if (type->isa<Single>()) return true;
    if (Idx::isa_lit(type) == 1) return true;
    if (analysis_.pinned(type)) return false;
    if (auto variant = lone(type)) return is_gone(variant->op(0));
    // An immutable aggregate is information-free iff all of its components are; this also covers `[]`.
    if (auto sigma = type->isa_imm<Sigma>())
        return std::ranges::all_of(sigma->ops(), [this](const Def* op) { return is_gone(op); });
    if (auto arr = type->isa_imm<Arr>()) return is_gone(arr->body());
    return false;
}

const Variant* SingleErasure::lone(const Def* type) {
    if (auto variant = type->isa<Variant>(); variant && variant->num_ops() == 1 && !analysis_.pinned(variant))
        if (variant->isa_imm() || variant->as_mut()->is_immutabilizable()) return variant;
    return nullptr;
}

Sieve SingleErasure::sieve(const Sigma* sigma) {
    // A foreign declaration has no components to inspect; a pinned one must keep its arity.
    if (!sigma->is_set() || analysis_.pinned(sigma)) return Sieve(sigma->num_ops());
    return Sieve(sigma->ops(), [this](const Def* op) { return !is_gone(op); });
}

const Def* SingleErasure::erase(const Single* single) {
    if (is_unit(single)) return new_world().sigma();
    return rewrite(single->op())->unfold_type();
}

const Def* SingleErasure::inhabitant(const Def* type) {
    if (auto single = type->isa<Single>(); single && !is_unit(single)) return rewrite(single->op());
    if (Idx::isa_lit(type) == 1) return new_world().lit_idx(1, 0);
    return new_world().tuple();
}

size_t SingleErasure::remap(const Sieve& keep, const Def* index) {
    auto i = Lit::isa(index);
    if (!i)
        index->blame("index is not a literal")
            .n("dropping an information-free component shifts the indices of its successors")
            .bail();

    auto j = keep[*i];
    assert(j != Sieve::Gone && "an Extract/Insert of a dropped component is erased before it is remapped");
    return j;
}

const Def* SingleErasure::rewrite_imm_Single(const Single* single) {
    auto type = erase(single);
    log().d("single-erasure `{}` → `{}`", single, type);
    profile_count("singletons eliminated");
    return type;
}

const Def* SingleErasure::rewrite_imm_Narrow(const Narrow* narrow) {
    profile_count("singletons eliminated");
    return inhabitant(narrow->type());
}

const Def* SingleErasure::rewrite_imm_Variant(const Variant* variant) {
    if (lone(variant)) return rewrite(variant->op(0));
    return RWPhase::rewrite_imm_Variant(variant);
}

const Def* SingleErasure::rewrite_imm_Inj(const Inj* inj) {
    if (lone(inj->type())) return rewrite(inj->value());
    return RWPhase::rewrite_imm_Inj(inj);
}

const Def* SingleErasure::rewrite_imm_Match(const Match* match) {
    if (lone(match->scrutinee()->unfold_type()))
        return new_world().app(rewrite(match->arm(0)), rewrite(match->scrutinee()));
    return RWPhase::rewrite_imm_Match(match);
}

const Def* SingleErasure::rewrite_imm_Sigma(const Sigma* sigma) {
    auto keep = sieve(sigma);
    if (keep.all()) return RWPhase::rewrite_imm_Sigma(sigma);
    return new_world().sigma(rewrite(keep.gather(sigma->ops())));
}

const Def* SingleErasure::rewrite_mut_Sigma(Sigma* old_sigma) {
    if (old_sigma->is_immutabilizable()) return rewrite_imm_Sigma(old_sigma);

    auto keep = sieve(old_sigma);
    if (keep.all()) return RWPhase::rewrite_mut_Sigma(old_sigma);

    auto new_sigma = new_world().mut_sigma(rewrite(old_sigma->type()), keep.num_new());
    return rewrite_stub(old_sigma, new_sigma, keep.new2old());
}

const Def* SingleErasure::rewrite_imm_Tuple(const Tuple* tuple) {
    if (is_gone(tuple->type())) return inhabitant(tuple->type());

    // Drive the drop by the *type*: a dependent component may reduce to a gone type while its Sigma op does not.
    if (auto sigma = tuple->type()->isa<Sigma>()) {
        auto keep = sieve(sigma);
        if (!keep.all()) return new_world().tuple(rewrite(tuple->type()), rewrite(keep.gather(tuple->ops())));
    }

    return RWPhase::rewrite_imm_Tuple(tuple);
}

const Def* SingleErasure::rewrite_imm_Extract(const Extract* extract) {
    if (is_gone(extract->type())) return inhabitant(extract->type());

    if (auto sigma = extract->tuple()->type()->isa<Sigma>()) {
        auto keep = sieve(sigma);
        if (!keep.all()) return new_world().extract(rewrite(extract->tuple()), remap(keep, extract->index()));
    }

    return RWPhase::rewrite_imm_Extract(extract);
}

const Def* SingleErasure::rewrite_imm_Insert(const Insert* insert) {
    if (is_gone(insert->value()->type())) return rewrite(insert->tuple());

    if (auto sigma = insert->tuple()->type()->isa<Sigma>()) {
        auto keep = sieve(sigma);
        if (!keep.all())
            return new_world().insert(rewrite(insert->tuple()), remap(keep, insert->index()), rewrite(insert->value()));
    }

    return RWPhase::rewrite_imm_Insert(insert);
}

const Def* SingleErasure::rewrite_imm_Seq(const Seq* seq) {
    if (is_gone(seq->is_intro() ? seq->type() : seq)) return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_imm_Seq(seq);
}

const Def* SingleErasure::rewrite_mut_Seq(Seq* seq) {
    // A mutable Seq has no immutable type to ask, so inspect its body directly.
    if (seq->is_set() && !analysis_.pinned(seq->is_intro() ? seq->type() : seq)
        && is_gone(seq->is_intro() ? seq->body()->type() : seq->body()))
        return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_mut_Seq(seq);
}

} // namespace mim
