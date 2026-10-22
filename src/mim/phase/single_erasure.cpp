#include "mim/phase/single_erasure.h"

namespace mim {

/*
 * SingleErasure
 */

Sieve SingleErasure::sieve(const Sigma* sigma) {
    // A foreign declaration has no components to inspect.
    if (!sigma->is_set()) return Sieve(sigma->num_ops());
    return Sieve(sigma->ops(), [this](const Def* op) { return !old_world().inhabitant(op); });
}

const Def* SingleErasure::erase(const Single* single) {
    if (is_unit(single)) return new_world().sigma();
    return rewrite(single->op())->unfold_type();
}

const Def* SingleErasure::inhabitant(const Def* type) {
    if (auto single = type->isa<Single>(); single && !is_unit(single)) return rewrite(single->op());
    return new_world().inhabitant(rewrite(type));
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

const Def* SingleErasure::rewrite_imm_Wrap(const Wrap* wrap) {
    profile_count("singletons eliminated");
    return inhabitant(wrap->type());
}

const Def* SingleErasure::rewrite_imm_Variant(const Variant* variant) {
    if (auto lone = Variant::isa_lone(variant)) return rewrite(lone);
    return RWPhase::rewrite_imm_Variant(variant);
}

const Def* SingleErasure::rewrite_imm_Inj(const Inj* inj) {
    if (Variant::isa_lone(inj->type())) return rewrite(inj->value());
    return RWPhase::rewrite_imm_Inj(inj);
}

const Def* SingleErasure::rewrite_imm_Match(const Match* match) {
    if (Variant::isa_lone(match->scrutinee()->unfold_type()))
        return new_world().app(rewrite(match->arm(0)), rewrite(match->scrutinee()));
    return RWPhase::rewrite_imm_Match(match);
}

const Def* SingleErasure::rewrite_imm_Sigma(const Sigma* sigma) {
    auto keep = sieve(sigma);
    if (keep.all()) return RWPhase::rewrite_imm_Sigma(sigma);
    return new_world().sigma(rewrite(keep.gather(sigma->ops())));
}

const Def* SingleErasure::rewrite_mut_Sigma(Sigma* old_sigma) {
    auto keep = sieve(old_sigma);
    if (keep.all()) return RWPhase::rewrite_mut_Sigma(old_sigma);

    auto new_sigma = new_world().mut_sigma(rewrite(old_sigma->type()), keep.num_new());
    return rewrite_stub(old_sigma, new_sigma, keep.new2old());
}

const Def* SingleErasure::rewrite_imm_Tuple(const Tuple* tuple) {
    if (old_world().inhabitant(tuple->type())) return inhabitant(tuple->type());
    return RWPhase::rewrite_imm_Tuple(tuple);
}

const Def* SingleErasure::rewrite_imm_Extract(const Extract* extract) {
    if (old_world().inhabitant(extract->type())) return inhabitant(extract->type());

    if (auto sigma = extract->tuple()->type()->isa<Sigma>()) {
        auto keep = sieve(sigma);
        if (!keep.all())
            return new_world().extract(rewrite(extract->tuple()), keep.num_new(), remap(keep, extract->index()));
    }

    return RWPhase::rewrite_imm_Extract(extract);
}

const Def* SingleErasure::rewrite_imm_Insert(const Insert* insert) {
    if (old_world().inhabitant(insert->value()->type())) return rewrite(insert->tuple());

    if (auto sigma = insert->tuple()->type()->isa<Sigma>()) {
        auto keep = sieve(sigma);
        if (!keep.all())
            return new_world().insert(rewrite(insert->tuple()), keep.num_new(), remap(keep, insert->index()),
                                      rewrite(insert->value()));
    }

    return RWPhase::rewrite_imm_Insert(insert);
}

const Def* SingleErasure::rewrite_imm_Seq(const Seq* seq) {
    if (old_world().inhabitant(seq->is_intro() ? seq->type() : seq)) return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_imm_Seq(seq);
}

const Def* SingleErasure::rewrite_mut_Seq(Seq* seq) {
    // A mutable Seq has no immutable type to ask, so inspect its body directly.
    if (seq->is_set() && old_world().inhabitant(seq->is_intro() ? seq->body()->type() : seq->body()))
        return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_mut_Seq(seq);
}

} // namespace mim
