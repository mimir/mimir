#include "mim/check.h"

#include <absl/container/fixed_array.h>
#include <fe/assert.h>

#include "mim/rewrite.h"
#include "mim/rule.h"
#include "mim/world.h"

namespace mim {

bool Def::needs_zonk() const {
    if (has_dep(Dep::Hole)) {
        for (auto mut : local_muts())
            if (Hole::isa_set(mut)) return true;
    }

    return false;
}

const Def* Def::zonk() const { return needs_zonk() ? world().zonker().rewrite(this) : this; }

const Def* Def::zonk_mut() const {
    if (!is_set()) return this;

    if (auto mut = isa_mut()) {
        if (auto hole = mut->isa<Hole>()) {
            auto [last, op] = hole->find();
            return op ? op->zonk() : last;
        }

        for (auto def : deps())
            if (def->needs_zonk()) return world().zonker().rewire_mut(mut);

        if (auto imm = mut->immutabilize()) return imm;
        return this;
    }

    return zonk();
}

DefVec Def::zonk(Defs defs) {
    return DefVec(defs.size(), [defs](size_t i) { return defs[i]->zonk(); });
}

/*
 * Hole
 */

std::pair<Hole*, const Def*> Hole::find() {
    auto def  = Def::op(0);
    auto last = this;

    while (def) {
        if (auto h = def->isa_mut<Hole>()) {
            def  = h->op();
            last = h;
        } else {
            break;
        }
    }

    auto root = def ? def : last;

    // path compression
    for (auto h = this; h != last;) {
        auto next = h->op()->as_mut<Hole>();
        h->set(root);
        h = next;
    }

    return {last, def};
}

const Def* Hole::tuplefy(nat_t n) {
    if (is_set()) return this;

    auto& w    = world();
    auto holes = absl::FixedArray<const Def*>(n);
    if (auto [sigma, var] = type()->isa_binder<Sigma>(); sigma && n >= 1) {
        auto rw  = VarRewriter(var, this);
        holes[0] = w.mut_hole(sigma->op(0));
        for (size_t i = 1; i != n; ++i) {
            rw.map(sigma->var(n, i - 1), holes[i - 1]);
            holes[i] = w.mut_hole(rw.rewrite(sigma->op(i)));
        }
    } else {
        for (size_t i = 0; i != n; ++i)
            holes[i] = w.mut_hole(type()->proj(n, i));
    }

    auto tuple = w.tuple(holes);
    set(tuple);
    return tuple;
}

/*
 * Checker
 */

#ifdef MIM_ENABLE_CHECKS
template<Checker::Mode mode>
bool Checker::fail() {
    if (mode == Check && world().flags().break_on_alpha) fe::breakpoint();
    return false;
}

const Def* Checker::fail() {
    if (world().flags().break_on_alpha) fe::breakpoint();
    return {};
}
#endif

const Def* Checker::is_uniform(Defs defs) {
    if (defs.empty()) return nullptr;
    auto first = defs.front();
    for (size_t i = 1, e = defs.size(); i != e; ++i)
        if (!alpha<Test>(first, defs[i])) return nullptr;
    return first;
}

const Def* Checker::assignable_(const Def* type, const Def* val) {
    auto val_ty = val->unfold_type()->zonk();
    if (type == val_ty) return val;

    auto& w = world();
    if (auto sigma = type->isa<Sigma>()) {
        if (!alpha_<Check>(type->arity(), val_ty->arity())) return fail();

        size_t a     = sigma->num_ops();
        auto red     = sigma->reduce(val);
        auto new_ops = absl::FixedArray<const Def*>(red.size());
        for (size_t i = 0; i != a; ++i) {
            auto new_val = assignable_(red[i], val->proj(a, i));
            if (new_val)
                new_ops[i] = new_val;
            else
                return fail();
        }
        return w.tuple(new_ops);
    } else if (auto uniq = val_ty->isa<Uniq>()) {
        if (auto new_val = assignable(type, uniq->op())) return new_val;
        return fail();
    }

    return alpha_<Check>(type, val_ty) ? val : fail();
}

template<Checker::Mode mode>
bool Checker::alpha_(const Def* d1, const Def* d2) {
    for (bool todo = true; todo;) {
        // below we check type and arity which may in turn open up more opportunities for zonking
        todo = false;
        d1   = d1->zonk_mut();
        d2   = d2->zonk_mut();

        // It is only safe to check for pointer equality if there are no Vars involved.
        // Otherwise, we have to look more thoroughly.
        // Example: λx.x - λz.x
        if (!d1->has_dep(Dep::Var) && !d2->has_dep(Dep::Var) && d1 == d2) return true;

        auto h1 = d1->isa_mut<Hole>();
        auto h2 = d2->isa_mut<Hole>();

        if constexpr (mode == Check) {
            if (h1) return h1->set(d2), true;
            if (h2) return h2->set(d1), true;
        } else if (h1 || h2) // mode == Test and h1 or h2 is an unresolved Hole
            return fail<Test>();

        if (!d1->is_set() || !d2->is_set()) return fail<mode>();

        auto mut1 = d1->isa_mut();
        auto mut2 = d2->isa_mut();

        if (mut1 && mut2 && mut1 == mut2) return true;

        // Globals are HACKs and require additionaly HACKs:
        // Unless they are pointer equal (above) always consider them unequal.
        if (d1->isa<Global>() || d2->isa<Global>()) return false;

        if (auto [i, ins] = bind(mut1, d2); !ins) return i->second == d2;
        if (auto [i, ins] = bind(mut2, d1); !ins) return i->second == d1;

        if (d1->isa<Top>() || d2->isa<Top>()) return mode == Check;

        if (auto t1 = d1->type()) {
            if (auto t2 = d2->type()) {
                if (!alpha_<mode>(t1, t2)) return fail<mode>();
            }
        }

        if (!alpha_<mode>(d1->arity(), d2->arity())) return fail<mode>();

        auto new_d1 = d1->zonk_mut();
        auto new_d2 = d2->zonk_mut();
        if (new_d1 != d1 || new_d2 != d2) {
            todo = true;
            d1   = new_d1;
            d2   = new_d2;
        }
    }

    auto seq1 = d1->isa<Seq>();
    auto seq2 = d2->isa<Seq>();

    if constexpr (mode == Mode::Check) {
        if (auto umax = d1->isa<UMax>(); umax && !d2->isa<UMax>()) return check(umax, d2);
        if (auto umax = d2->isa<UMax>(); umax && !d1->isa<UMax>()) return check(umax, d1);

        if (seq1 && seq1->arity() == world().lit_nat_1() && !seq2) return check1(seq1, d2);
        if (seq2 && seq2->arity() == world().lit_nat_1() && !seq1) return check1(seq2, d1);

        if (seq1 && seq2) {
            if (auto mut_seq = seq1->isa_mut<Seq>(); mut_seq && seq2->isa_imm()) return check(mut_seq, seq2);
            if (auto mut_seq = seq2->isa_mut<Seq>(); mut_seq && seq1->isa_imm()) return check(mut_seq, seq1);
        }
    }

    if (auto prod = d1->isa<Prod>()) return check<mode>(prod, d2);
    if (auto prod = d2->isa<Prod>()) return check<mode>(prod, d1);
    if (seq1 && seq2) return alpha_<mode>(seq1->body(), seq2->body());

    if (d1->node() != d2->node() || d1->flags() != d2->flags()) return fail<mode>();

    if (auto var1 = d1->isa<Var>()) {
        auto var2 = d2->as<Var>();
        if (auto i = binders_.find(var1->mut()); i != binders_.end()) return i->second == var2->mut();
        if (auto i = binders_.find(var2->mut()); i != binders_.end()) return fail<mode>(); // var2 is bound
        // both var1 and var2 are free: OK, when they are the same or in Check mode
        return var1 == var2 || mode == Check;
    }

    for (size_t i = 0, e = d1->num_ops(); i != e; ++i)
        if (!alpha_<mode>(d1->op(i), d2->op(i))) return fail<mode>();
    return true;
}

template<Checker::Mode mode>
bool Checker::check(const Prod* prod, const Def* def) {
    size_t a = prod->num_ops();
    for (size_t i = 0; i != a; ++i)
        if (!alpha_<mode>(prod->op(i), def->proj(a, i))) return fail<mode>();
    return true;
}

// alpha(«1; body», def) -> alpha(body, def);
bool Checker::check1(const Seq* seq, const Def* def) {
    auto body = seq->reduce(world().lit_idx_1_0()); // try to get rid of var inside of body
    if (!alpha_<Check>(body, def)) return fail<Check>();
    if (auto mut_seq = seq->isa_mut<Seq>()) mut_seq->set(world().lit_nat_1(), body->zonk());
    return true;
}

// Try to get rid of mut_seq's var: it may occur in its body and vanish after reduction
// as holes might have been filled in the meantime.
bool Checker::check(Seq* mut_seq, const Seq* imm_seq) {
    auto mut_body = mut_seq->reduce(world().top(world().type_idx(mut_seq->arity())));
    if (!alpha_<Check>(mut_body, imm_seq->body())) return fail<Check>();

    mut_seq->set(mut_seq->arity(), mut_body->zonk());
    return true;
}

bool Checker::check(const UMax* umax, const Def* def) {
    for (auto op : umax->ops())
        if (!alpha<Check>(op, def)) return fail<Check>();
    return true;
}

/*
 * infer & check
 */

const Def* Arr::check(size_t, const Def* def) { return def; } // TODO

const Def* Arr::check() {
    auto t = body()->unfold_type();
    if (!Checker::alpha<Checker::Check>(t, type()))
        error(type()->loc(), "declared sort '{}' of array does not match inferred one '{}'", type(), t);
    return t;
}

const Def* Tuple::infer(World& world, Defs ops) {
    auto elems = absl::FixedArray<const Def*>(ops.size());
    for (size_t i = 0, e = ops.size(); i != e; ++i)
        elems[i] = ops[i]->unfold_type();
    return world.sigma(elems);
}

const Def* Sigma::infer(World& w, Defs ops) {
    auto elems = absl::FixedArray<const Def*>(ops.size());
    for (size_t i = 0, e = ops.size(); i != e; ++i)
        elems[i] = ops[i]->unfold_type();
    return w.umax<UMax::Kind>(elems);
}

const Def* Sigma::check(size_t, const Def* def) { return def; } // TODO

const Def* Sigma::check() {
    auto t = infer(world(), ops());
    if (t != type()) {
        // TODO HACK
        if (Checker::alpha<Checker::Check>(t, type()))
            return t;
        else {
            world().WLOG(
                "incorrect type '{}' for '{}'. Correct one would be: '{}'. I'll keep this one nevertheless due to "
                "bugs in clos-conv",
                type(), this, t);
            return type();
        }
    }
    return t;
}

const Def* Pi::infer(const Def* dom, const Def* codom) {
    auto& w = dom->world();
    return w.umax<UMax::Kind>({dom->unfold_type(), codom->unfold_type()});
}

const Def* Pi::check(size_t, const Def* def) { return def; }

const Def* Pi::check() {
    auto t = infer(dom(), codom());
    if (!Checker::alpha<Checker::Check>(t, type()))
        error(type()->loc(), "declared sort '{}' of function type does not match inferred one '{}'", type(), t);
    return t;
}

const Def* Lam::check(size_t i, const Def* def) {
    if (i == 0) {
        if (auto filter = Checker::assignable(world().type_bool(), def)) return filter;
        throw Error().error(filter()->loc(), "filter '{}' of lambda is of type '{}' but must be of type 'Bool'",
                            filter(), filter()->type());
    }
    assert(i == 1);
    if (auto body = Checker::assignable(codom(), def)) return body;
    throw Error()
        .error(def->loc(), "body of function is not assignable to declared codomain")
        .note(def->loc(), "body: '{}'", def)
        .note(def->loc(), "type: '{}'", def->type())
        .note(codom()->loc(), "codomain: '{}'", codom());
}

const Def* Reform::check() {
    auto t = infer(dom());
    if (!Checker::alpha<Checker::Check>(t, type()))
        error(type()->loc(), "declared sort '{}' of rule type does not match inferred one '{}'", type(), t);
    return t;
}

const Def* Reform::infer(const Def* dom) { return dom->unfold_type(); }

const Def* Rule::check() {
    auto t1 = lhs()->type();
    auto t2 = rhs()->type();
    if (!Checker::alpha<Checker::Check>(t1, t2))
        error(type()->loc(), "type mismatch: '{}' for lhs, but '{}' for rhs", t1, t2);
    if (!Checker::assignable(world().type_bool(), guard()))
        error(guard()->loc(), "condition '{}' of rewrite is of type '{}' but must be of type 'Bool'", guard(),
              guard()->type());

    return type();
}

const Def* Rule::check(size_t, const Def* def) {
    return def;
    // TODO: do actual check + what are the parameters ?
}

#ifndef DOXYGEN
template bool Checker::alpha_<Checker::Check>(const Def*, const Def*);
template bool Checker::alpha_<Checker::Test>(const Def*, const Def*);
#endif

} // namespace mim
