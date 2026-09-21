#include "mim/phase/single_erasure.h"

namespace mim {

/*
 * Analysis
 */

/// Collects @p def%'s immutable subtree into @p set; stops at mutables.
static void collect(DefSet& set, const Def* def) {
    if (!set.emplace(def).second) return;
    if (def->isa_mut()) return;
    for (auto d : def->deps())
        collect(set, d);
}

const Def* SingleErasure::Analysis::rewrite(const Def* old) {
    // Visit the subtree *before* inspecting: pin() seeds `def ↦ def` into the rewriter map, which would
    // short-circuit the traversal into `old` and skip its subtree for the rest of the round.
    auto res = mim::Analysis::rewrite(old);

    if (auto app = old->isa<App>()) {
        // Rebuilding an Axm application re-derives its shapes from the Axm's generic type instead of rewriting
        // them (`Idx n_groups` becomes `Idx 1` all over again), so whatever its signature dictates must keep its
        // arity. Subtrees merely *substituted* in via earlier (type) arguments impose no shape, though - they
        // rewrite consistently with the rest of the World: seed the walk's visited set with them.
        if (app->uncurry_callee()->isa<Axm>()) {
            auto skips = DefSet();
            for (auto d = old; auto a = d->isa<App>(); d = a->callee())
                collect(skips, a->arg());
            pin_tree(app->callee_type()->dom(), skips);
            pin_tree(app->type(), skips);
        }

        // Assignability is alpha-equivalence, so an App may connect a dom and an arg whose types are *distinct*
        // defs - a dependent dom and its instance (`[n: Nat, «n; *»]` vs `[Nat, []]`). Dropping decides per def,
        // which would tear such an edge apart: the instance loses what the dependent dom still expects.
        if (auto dom = app->callee_type()->dom(); dom != app->arg()->type()) {
            auto visited = DefSet();
            pin_tree(dom, visited);
            pin_tree(app->arg()->type(), visited);
        }
    }

    return res;
}

void SingleErasure::Analysis::pin_tree(const Def* def, DefSet& visited) {
    if (!visited.emplace(def).second) return;
    if (def->isa<Sigma>() || def->isa<Arr>()) pin(def);
    for (auto d : def->deps())
        pin_tree(d, visited);
}

/*
 * SingleErasure
 */

bool SingleErasure::is_gone(const Def* type) {
    if (type->isa<Single>()) return true;
    if (Idx::isa_lit(type) == 1) return true;
    if (analysis_.pinned(type)) return false;
    // An immutable aggregate is information-free iff all of its components are; this also covers `[]`.
    if (auto sigma = type->isa_imm<Sigma>())
        return std::ranges::all_of(sigma->ops(), [this](const Def* op) { return is_gone(op); });
    if (auto arr = type->isa_imm<Arr>()) return is_gone(arr->body());
    return false;
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
        index->blame("index `{}` is not a literal", index)
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

const Def* SingleErasure::rewrite_imm_Wrap(const Wrap* wrap) {
    profile_count("singletons eliminated");
    return inhabitant(wrap->type());
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
