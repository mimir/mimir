#include "mim/rewrite.h"

#include <absl/container/fixed_array.h>

#include "mim/world.h"

#include "fe/assert.h"

// Don't use fancy C++-lambdas; it's way too annoying stepping through them in a debugger.

namespace mim {

/*
 * Rewriter
 */

const Def* Rewriter::map(const Def* old_def, Defs new_defs) {
    return old2news_.back()[old_def] = world().tuple(new_defs);
}
const Def* Rewriter::map(Defs old_defs, const Def* new_def) {
    return old2news_.back()[world().tuple(old_defs)] = new_def;
}
const Def* Rewriter::map(Defs old_defs, Defs new_defs) {
    return old2news_.back()[world().tuple(old_defs)] = world().tuple(new_defs);
}

const Def* Rewriter::rewrite(const Def* old_def) {
    if (auto new_def = lookup(old_def)) return new_def;

    auto new_def = old_def->isa_mut() ? rewrite_mut((Def*)old_def) : rewrite_imm(old_def);
    return new_def->set(old_def->dbg());
}

// clang-format off
#define CODE_MUT(N) case Node::N: new_def = rewrite_mut_##N(old_mut->as<N>()); break;
#define CODE_IMM(N) case Node::N: new_def = rewrite_imm_##N(old_def->as<N>()); break;
// clang-format on

const Def* Rewriter::rewrite_imm(const Def* old_def) {
    const Def* new_def;
    switch (old_def->node()) {
        MIM_IMM_NODE(CODE_IMM)
        default: fe::unreachable();
    }
    return map(old_def, new_def);
}

const Def* Rewriter::rewrite_mut(Def* old_mut) {
    const Def* new_def;
    switch (old_mut->node()) {
        MIM_MUT_NODE(CODE_MUT)
        default: fe::unreachable();
    }
    return new_def;
}

#undef CODE_MUT
#undef CODE_IMM

DefVec Rewriter::rewrite(Defs ops) {
    auto new_ops = DefVec(ops.size());
    for (size_t i = 0, e = ops.size(); i != e; ++i)
        new_ops[i] = rewrite(ops[i]);
    return new_ops;
}

#ifndef DOXYGEN
// clang-format off
const Def* Rewriter::rewrite_imm_Idx   (const Idx*     ) { return world().type_idx(); }
const Def* Rewriter::rewrite_imm_Nat   (const Nat*     ) { return world().type_nat(); }
const Def* Rewriter::rewrite_imm_Univ  (const Univ*    ) { return world().univ();     }
const Def* Rewriter::rewrite_imm_App   (const App*    d) { return world().app   (rewrite(d->callee()), rewrite(d->arg()));                                         }
const Def* Rewriter::rewrite_imm_Inj   (const Inj*    d) { return world().inj   (rewrite(d->type()),   rewrite(d->value()));                                       }
const Def* Rewriter::rewrite_imm_Insert(const Insert* d) { return world().insert(rewrite(d->tuple()),  rewrite(d->index()), rewrite(d->value()));                  }
const Def* Rewriter::rewrite_imm_Lam   (const Lam*    d) { return world().lam   (rewrite(d->type())->as<Pi>(), rewrite(d->filter()), rewrite(d->body()));          }
const Def* Rewriter::rewrite_imm_Lit   (const Lit*    d) { return world().lit   (rewrite(d->type()),   d->get());                                                  }
const Def* Rewriter::rewrite_imm_Match (const Match*  d) { return world().match (rewrite(d->ops()));                                                               }
const Def* Rewriter::rewrite_imm_Merge (const Merge*  d) { return world().merge (rewrite(d->type()),   rewrite(d->ops()));                                         }
const Def* Rewriter::rewrite_imm_Pi    (const Pi*     d) { return world().pi    (rewrite(d->dom()),    rewrite(d->codom()), d->is_implicit());                     }
const Def* Rewriter::rewrite_imm_Proxy (const Proxy*  d) { return world().proxy (rewrite(d->type()),   rewrite(d->ops()), d->pass(), d->tag());                    }
const Def* Rewriter::rewrite_imm_Reform(const Reform* d) { return world().reform(rewrite(d->meta_type()));                                                         }
const Def* Rewriter::rewrite_imm_Rule  (const Rule*   d) { return world().rule  (rewrite(d->type()),   rewrite(d->lhs()), rewrite(d->rhs()), rewrite(d->guard())); }
const Def* Rewriter::rewrite_imm_Sigma (const Sigma*  d) { return world().sigma (rewrite(d->ops()));                                                               }
const Def* Rewriter::rewrite_imm_Split (const Split*  d) { return world().split (rewrite(d->type()),   rewrite(d->value()));                                       }
const Def* Rewriter::rewrite_imm_Tuple (const Tuple*  d) { return world().tuple (rewrite(d->type()),   rewrite(d->ops()));                                         }
const Def* Rewriter::rewrite_imm_Type  (const Type*   d) { return world().type  (rewrite(d->level()));                                                             }
const Def* Rewriter::rewrite_imm_UInc  (const UInc*   d) { return world().uinc  (rewrite(d->op()),     d->offset());                                               }
const Def* Rewriter::rewrite_imm_UMax  (const UMax*   d) { return world().umax  (rewrite(d->ops()));                                                               }
const Def* Rewriter::rewrite_imm_Uniq  (const Uniq*   d) { return world().uniq  (rewrite(d->op()));                                                                }
const Def* Rewriter::rewrite_imm_Var   (const Var*    d) { return world().var   (rewrite(d->mut())->as_mut());                                                     }
const Def* Rewriter::rewrite_imm_Top   (const Top*    d) { return world().top   (rewrite(d->type()));                                                              }
const Def* Rewriter::rewrite_imm_Bot   (const Bot*    d) { return world().bot   (rewrite(d->type()));                                                              }
const Def* Rewriter::rewrite_imm_Meet  (const Meet*   d) { return world().meet  (rewrite(d->ops()));                                                               }
const Def* Rewriter::rewrite_imm_Join  (const Join*   d) { return world().join  (rewrite(d->ops()));                                                               }

const Def* Rewriter::rewrite_imm_Arr (const Arr*  d) { return rewrite_imm_Seq(d); }
const Def* Rewriter::rewrite_imm_Pack(const Pack* d) { return rewrite_imm_Seq(d); }
const Def* Rewriter::rewrite_mut_Arr (      Arr*  d) { return rewrite_mut_Seq(d); }
const Def* Rewriter::rewrite_mut_Pack(      Pack* d) { return rewrite_mut_Seq(d); }

const Def* Rewriter::rewrite_mut_Pi    (Pi*     d) { return rewrite_stub(d, world().mut_pi   (rewrite(d->type()), d->is_implicit())); }
const Def* Rewriter::rewrite_mut_Lam   (Lam*    d) { return rewrite_stub(d, world().mut_lam  (rewrite(d->type())->as<Pi>()));         }
const Def* Rewriter::rewrite_mut_Rule  (Rule*   d) { return rewrite_stub(d, world().mut_rule (rewrite(d->type())->as<Reform>()));     }
const Def* Rewriter::rewrite_mut_Sigma (Sigma*  d) { return rewrite_stub(d, world().mut_sigma(rewrite(d->type()), d->num_ops()));     }
const Def* Rewriter::rewrite_mut_Global(Global* d) { return rewrite_stub(d, world().global   (rewrite(d->type()), d->is_mutable()));  }
// clang-format on

const Def* Rewriter::rewrite_imm_Axm(const Axm* a) {
    if (&a->world() != &world()) {
        auto type = rewrite(a->type());
        return world().axm(a->normalizer(), a->curry(), a->trip(), type, a->plugin(), a->tag(), a->sub());
    }
    return a;
}

const Def* Rewriter::rewrite_imm_Extract(const Extract* ex) {
    auto new_index = rewrite(ex->index());
    if (auto index = Lit::isa(new_index)) {
        if (auto tuple = ex->tuple()->isa<Tuple>()) return map(ex, rewrite(tuple->op(*index)));
        if (auto pack = ex->tuple()->isa_imm<Pack>(); pack && pack->arity()->is_closed())
            return map(ex, rewrite(pack->body()));
    }

    auto new_tuple = rewrite(ex->tuple());
    return world().extract(new_tuple, new_index);
}

const Def* Rewriter::rewrite_mut_Hole(Hole* hole) {
    auto [last, op] = hole->find();
    return op ? rewrite(op) : rewrite_stub(last, world().mut_hole(rewrite(last->type())));
}

#endif

const Def* Rewriter::rewrite_imm_Seq(const Seq* seq) {
    auto new_arity = rewrite(seq->arity());
    if (auto l = Lit::isa(new_arity); l && *l == 0) return world().prod(seq->is_intro());
    return world().seq(seq->is_intro(), new_arity, rewrite(seq->body()));
}

const Def* Rewriter::rewrite_mut_Seq(Seq* seq) {
    if (!seq->is_set()) {
        auto new_seq = seq->as_mut<Seq>()->stub(world(), rewrite(seq->type()));
        return map(seq, new_seq);
    }

    auto new_arity = rewrite(seq->arity())->zonk();
    auto l         = Lit::isa(new_arity);
    if (l && *l == 0) return world().prod(seq->is_intro());

    if (auto var = seq->has_var(); var && l && *l <= world().flags().scalarize_threshold) {
        auto new_ops = absl::FixedArray<const Def*>(*l);
        for (size_t i = 0, e = *l; i != e; ++i) {
            push();
            map(var, world().lit_idx(e, i));
            new_ops[i] = rewrite(seq->body());
            pop();
        }
        return map(seq, world().prod(seq->is_intro(), new_ops));
    }

    if (!seq->has_var()) return map(seq, world().seq(seq->is_intro(), new_arity, rewrite(seq->body())));
    return rewrite_stub(seq->as_mut(), world().mut_seq(seq->is_intro(), rewrite(seq->type())));
}

const Def* Rewriter::rewrite_stub(Def* old_mut, Def* new_mut) {
    map(old_mut, new_mut);

    if (old_mut->is_set()) {
        for (size_t i = 0, e = old_mut->num_ops(); i != e; ++i)
            new_mut->set(i, rewrite(old_mut->op(i)));
        if (auto new_imm = new_mut->immutabilize()) return map(old_mut, new_imm);
    }

    return new_mut;
}

/*
 * VarRewriter
 */

const Def* VarRewriter::rewrite(const Def* old_def) {
    if (auto new_def = lookup(old_def)) return new_def;

    if (auto old_mut = old_def->isa_mut())
        return has_intersection(old_mut) ? rewrite_mut(old_mut)->set(old_mut->dbg()) : old_mut;

    if (old_def->local_vars().empty() && old_def->local_muts().empty()) return old_def; // safe to skip

    return has_intersection(old_def) ? rewrite_imm(old_def)->set(old_def->dbg()) : old_def;
}

const Def* VarRewriter::rewrite_mut(Def* mut) {
    if (auto var = mut->has_var()) {
        auto& vars = vars_.back();
        vars       = world().vars().insert(vars, var);
    }

    return Rewriter::rewrite_mut(mut);
}

/*
 * Zonker
 */

const Def* Zonker::map(const Def* old_def, const Def* new_def) {
    auto repr = lookup(new_def); // always normalize new_def to its representative
    if (!repr) repr = new_def;
    return old2news_.back()[old_def] = repr;
}

const Def* Zonker::lookup(const Def* old_def) {
    for (auto& old2new : old2news_ | std::views::reverse) {
        const Def* repr;
        auto path = DefVec();
        while (true) {
            repr = get(old_def);

            if (repr == nullptr) break;

            path.emplace_back(repr);
            if (repr == old_def) break; // explicit self-map

            old_def = repr;
        }

        if (path.empty()) continue;

        // path compression: flatten all visited nodes
        for (auto def : path)
            old2new[def] = repr;

        return repr;
    }

    return nullptr;
}

const Def* Zonker::rewrite(const Def* def) {
    if (auto hole = def->isa_mut<Hole>()) {
        auto [last, op] = hole->find();
        def             = op ? op : last;
    }

    return def->needs_zonk() ? Rewriter::rewrite(def) : def;
}

const Def* Zonker::rewire_mut(Def* mut) {
    map(mut, mut);

    auto old_type = mut->type();
    auto old_ops  = absl::FixedArray<const Def*>(mut->ops().begin(), mut->ops().end());

    mut->unset()->set_type(rewrite(old_type));

    for (size_t i = 0, e = mut->num_ops(); i != e; ++i)
        mut->set(i, rewrite(old_ops[i]));

    if (auto new_imm = mut->immutabilize()) return map(mut, new_imm);

    return mut;
}

} // namespace mim
