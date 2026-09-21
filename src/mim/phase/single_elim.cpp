#include "mim/phase/single_elim.h"

namespace mim {

const Def* SingleElim::erase(const Single* single) {
    if (is_unit(single)) return new_world().sigma();
    return rewrite(single->op())->unfold_type();
}

const Def* SingleElim::inhabitant(const Single* single) {
    if (is_unit(single)) return new_world().tuple();
    return rewrite(single->op());
}

size_t SingleElim::num_gone(const Sigma* sigma) {
    if (!sigma->is_set()) return 0;
    return std::ranges::count_if(sigma->ops(), is_gone);
}

size_t SingleElim::remap(const Sigma* sigma, const Def* index) {
    auto i = Lit::isa(index);
    if (!i)
        index->blame("index `{}` into `{}` is not a literal", index, sigma)
            .n("dropping a singleton component shifts the indices of its successors")
            .bail();

    size_t j = 0;
    for (size_t k = 0; k != *i; ++k)
        if (!is_gone(sigma->op(k))) ++j;
    return j;
}

const Def* SingleElim::rewrite_imm_Single(const Single* single) {
    auto type = erase(single);
    log().d("single-elim `{}` → `{}`", single, type);
    profile_count("singletons eliminated");
    return type;
}

const Def* SingleElim::rewrite_imm_Wrap(const Wrap* wrap) {
    profile_count("singletons eliminated");
    return inhabitant(wrap->type()->as<Single>());
}

const Def* SingleElim::rewrite_imm_Sigma(const Sigma* sigma) {
    if (num_gone(sigma) == 0) return RWPhase::rewrite_imm_Sigma(sigma);

    auto new_ops = DefVec();
    for (auto op : sigma->ops())
        if (!is_gone(op)) new_ops.emplace_back(rewrite(op));
    return new_world().sigma(new_ops);
}

const Def* SingleElim::rewrite_mut_Sigma(Sigma* old_sigma) {
    if (old_sigma->is_immutabilizable()) return rewrite_imm_Sigma(old_sigma);

    auto gone = num_gone(old_sigma);
    if (gone == 0) return RWPhase::rewrite_mut_Sigma(old_sigma);

    auto new_sigma = new_world().mut_sigma(rewrite(old_sigma->type()), old_sigma->num_ops() - gone);
    map(old_sigma, new_sigma);
    auto _ = enter(old_sigma);
    for (size_t i = 0, j = 0, e = old_sigma->num_ops(); i != e; ++i)
        if (!is_gone(old_sigma->op(i))) new_sigma->set(j++, rewrite(old_sigma->op(i)));

    if (auto new_imm = new_sigma->immutabilize()) return map(old_sigma, new_imm);
    return new_sigma;
}

const Def* SingleElim::rewrite_imm_Tuple(const Tuple* tuple) {
    auto new_type = rewrite(tuple->type());
    auto new_ops  = DefVec();
    for (auto op : tuple->ops())
        if (!is_gone(op->type())) new_ops.emplace_back(rewrite(op));
    return new_world().tuple(new_type, new_ops);
}

const Def* SingleElim::rewrite_imm_Extract(const Extract* extract) {
    if (auto single = extract->type()->isa<Single>()) return inhabitant(single);

    if (auto sigma = extract->tuple()->type()->isa<Sigma>(); sigma && num_gone(sigma) != 0)
        return new_world().extract(rewrite(extract->tuple()), remap(sigma, extract->index()));

    return RWPhase::rewrite_imm_Extract(extract);
}

const Def* SingleElim::rewrite_imm_Insert(const Insert* insert) {
    if (is_gone(insert->value()->type())) return rewrite(insert->tuple());

    if (auto sigma = insert->tuple()->type()->isa<Sigma>(); sigma && num_gone(sigma) != 0)
        return new_world().insert(rewrite(insert->tuple()), remap(sigma, insert->index()), rewrite(insert->value()));

    return RWPhase::rewrite_imm_Insert(insert);
}

const Def* SingleElim::rewrite_imm_Seq(const Seq* seq) {
    if (is_gone(seq->is_intro() ? seq->body()->type() : seq->body())) return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_imm_Seq(seq);
}

const Def* SingleElim::rewrite_mut_Seq(Seq* seq) {
    if (seq->is_set() && is_gone(seq->is_intro() ? seq->body()->type() : seq->body()))
        return new_world().prod(seq->is_intro());
    return RWPhase::rewrite_mut_Seq(seq);
}

} // namespace mim
