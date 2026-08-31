#include "mim/check.h"

#include <fe/assert.h>

#include "mim/driver.h"
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

const Def* Def::zonk() const {
    // A Hole needs special care: even when it is still unset, its *type* may have to be refreshed; see zonk_mut.
    if (isa_mut<Hole>()) return zonk_mut();
    return needs_zonk() ? world().zonker().rewrite(this) : this;
}

const Def* Def::zonk_mut() const {
    if (auto hole = isa_mut<Hole>()) {
        auto [last, op] = hole->find();
        if (op) return op->zonk();
        // The Hole is still unset, but its *type* may mention Hole%s that have been resolved in the meantime.
        // Refresh it; otherwise e.g. an `«?n; T»` type won't collapse to `T` after `?n` has been unified with `1`.
        if (auto t = last->type())
            if (auto new_t = t->zonk_mut(); new_t != t) last->set_type(new_t);
        return last;
    }

    if (!is_set()) return this;

    if (auto mut = isa_mut()) {
        for (auto def : deps())
            if (def->needs_zonk()) return world().zonker().rewire_mut(mut);

        if (auto imm = mut->immutabilize()) return imm;
        return this;
    }

    return zonk();
}

DefVec Def::zonk(Defs defs) {
    return DefVec(defs, [](const Def* def) { return def->zonk(); });
}

/*
 * Hole
 */

std::pair<Hole*, const Def*> Hole::find() {
    auto def  = Def::op(0);
    auto last = this;

    while (def) {
        auto h = def->isa_mut<Hole>();
        if (!h) break;
        def  = h->op();
        last = h;
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
    auto holes = DefVec(n);
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
    auto same  = [first](const Def* def) { return alpha<Test>(first, def); };
    return std::ranges::all_of(defs.subspan(1), same) ? first : nullptr;
}

const Def* Checker::assignable_(const Def* type, const Def* val) {
    auto val_t = val->unfold_type();
    if (!val_t) return fail(); // Univ has no type, so it is assignable to nothing
    auto val_ty = val_t->zonk();
    if (type == val_ty) return val;

    auto& w = world();

    // Implicit insertion at a coercion site: @p val expects implicit arguments, but @p type is an
    // *explicit* function type. Fill them with Hole%s, as World::implicit_app does at application sites.
    // This lets polymorphic functions be passed as arguments, e.g. `%%affine.id` to a parameter of type
    // `«r; %%affine.Idx» → «r; %%affine.Idx»`, without writing `%%affine.id @r`.
    // Only do this when @p type is a Pi: if it is a Hole, we'd commit before knowing what's expected;
    // if it's an aggregate, we might consume implicits belonging to the value's element type.
    if (auto pi = type->isa<Pi>(); pi && !pi->is_implicit() && Pi::isa_implicit(val_ty)) {
        while (auto ipi = Pi::isa_implicit(val_ty)) {
            val    = w.app(val, w.mut_hole(ipi->dom()));
            val_ty = val->unfold_type()->zonk();
        }
        if (type == val_ty) return val;
    }

    if (auto sigma = type->isa<Sigma>()) {
        if (!alpha_<Check>(type->arity(), val_ty->arity())) return fail();

        size_t a     = sigma->num_ops();
        auto red     = sigma->reduce(val);
        auto new_ops = DefVec(a);
        for (size_t i = 0; i != a; ++i) {
            auto new_val = assignable_(red[i], val->proj(a, i));
            if (!new_val) return fail();
            new_ops[i] = new_val;
        }
        return w.tuple(new_ops);
    }

    if (auto uniq = val_ty->isa<Uniq>()) {
        if (auto new_val = assignable(type, uniq->op())) return new_val;
        return fail();
    }

    return alpha_<Check>(type, val_ty) ? val : fail();
}

std::pair<Checker::Binders::iterator, bool> Checker::bind(Def* mut, const Def* d) {
    if (!mut) return {binders_.end(), true};

    auto res = binders_.emplace(mut, d);
    if (res.second) {
        // A new binding may change how bound Var%s compare, so positive memo entries may become invalid.
        for (auto& memo : memo_)
            memo.clear();
        // A Var that has never been created cannot occur in any Def.
        if (auto var = mut->has_var()) bound_ = world().vars().insert(bound_, var);
    }

    return res;
}

/// Is @p def a Seq that spans exactly one dimension, i.e. one that a rank can be peeled off?
static bool isa_dim(const Def* def) {
    auto seq = def->isa<Seq>();
    return seq && seq->arity()->unfold_type()->zonk_mut()->isa<Nat>();
}

/// The rank of `«s; T»` with `s: «r; Nat»` is unknown as long as `r` is: World::seq cannot un-nest it yet.
/// @returns the unset Hole standing for `r`, or `nullptr`.
static Hole* isa_flex_rank(const Def* def) {
    if (auto seq = def->isa_imm<Seq>()) {
        if (auto shape = Hole::isa_unset(seq->arity()->zonk_mut())) {
            if (auto arr = shape->type()->zonk_mut()->isa<Arr>()) return Hole::isa_unset(arr->arity()->zonk_mut());
        }
    }
    return nullptr;
}

// These may be α-equivalent to a Def with a different Node or Def::flags(); see alpha_impl_.
static bool is_flex(const Def* def) {
    auto n = def->node();
    return n == Node::Hole || n == Node::Top || n == Node::UMax || Prod::isa_node(n) || Seq::isa_node(n);
}

template<Checker::Mode mode>
std::optional<bool> Checker::try_alpha_(const Def* d1, const Def* d2) {
    // Pointer equality decides the matter, unless a free Var of an immutable is bound on one side only: λx.x vs λz.x.
    if (d1 == d2 && (d1->isa_mut() || bound_.empty() || !d1->has_free_vars_in(bound_))) return true;

    // Only a ground Def is stable under Def::zonk_mut, which rewires mutables in place and unifies Hole%s.
    if ((d1->node() != d2->node() || d1->flags() != d2->flags()) && d1->is_ground() && d2->is_ground() && !is_flex(d1)
        && !is_flex(d2))
        return fail<mode>();

    return {};
}

template<Checker::Mode mode>
bool Checker::alpha_(const Def* d1, const Def* d2) {
    if (auto res = try_alpha_<mode>(d1, d2)) return *res;

    auto& memo = memo_[mode];
    auto key   = memo_key(d1, d2);
    if (memo.contains(key)) return true;
    if (!alpha_impl_<mode>(d1, d2)) return false;
    memo.emplace(key);
    return true;
}

template<Checker::Mode mode>
bool Checker::alpha_impl_(const Def* d1, const Def* d2) {
    for (bool todo = true; todo;) {
        // below we check type and arity which may in turn open up more opportunities for zonking
        todo = false;
        d1   = d1->zonk_mut();
        d2   = d2->zonk_mut();

        if (auto res = try_alpha_<mode>(d1, d2); res.has_value()) return *res;

        auto h1 = d1->isa_mut<Hole>();
        auto h2 = d2->isa_mut<Hole>();

        if constexpr (mode == Check) {
            if (h1) return check(h1, d2);
            if (h2) return check(h2, d1);
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

        auto t1 = d1->type();
        auto t2 = d2->type();
        if (t1 && t2 && !alpha_<mode>(t1, t2)) return fail<mode>();

        // The arity of a flex-rank Seq is the entire shape vector, so its rank must be pinned down first.
        if constexpr (mode == Check) {
            if (auto rank = isa_flex_rank(d1); rank && isa_dim(d2)) {
                if (!check_rank(d1->as<Seq>(), rank, d2)) return fail<Check>();
                todo = true;
                continue;
            }
            if (auto rank = isa_flex_rank(d2); rank && isa_dim(d1)) {
                if (!check_rank(d2->as<Seq>(), rank, d1)) return fail<Check>();
                todo = true;
                continue;
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

    if constexpr (mode == Check) {
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
        if (auto i = binders_.find(var1->binder()); i != binders_.end()) return i->second == var2->binder();
        if (auto i = binders_.find(var2->binder()); i != binders_.end()) return fail<mode>(); // var2 is bound
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

// A Hole may only be solved with a Def its type accepts; this is what pins `r` down in `s: «r; Nat»`.
bool Checker::check(Hole* hole, const Def* def) {
    if (def->unfold_type()) { // Univ has no type and is assignable to nothing
        if (auto new_def = assignable_(hole->type(), def))
            def = new_def;
        else
            return fail<Check>();
    }
    return hole->set(def), true;
}

// alpha(«?s; body», «e₀; «e₁; … «e_{n-1}; def»…»): peel dimensions until the remainder matches body.
// This determines the rank; Hole::tuplefy then hands the extents to the regular structural comparison.
bool Checker::check_rank(const Seq* seq, Hole* rank, const Def* def) {
    auto body  = seq->body();
    size_t n   = 0;
    size_t num = 0;

    for (size_t i = 1; isa_dim(def); ++i) {
        def = def->as<Seq>()->body();
        if (alpha_<Test>(body, def)) n = i, ++num; // Test mode: probing must not solve any Hole
    }

    if (num != 1) return fail<Check>(); // no rank fits, or several do and the call site needs an explicit `@`

    rank->set(world().lit_nat(n));
    Hole::isa_unset(seq->arity()->zonk_mut())->tuplefy(n);
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

#ifndef DOXYGEN
template bool Checker::alpha_<Checker::Check>(const Def*, const Def*);
template bool Checker::alpha_<Checker::Test>(const Def*, const Def*);
#endif

/*
 * infer
 */

const Def* Tuple::infer(World& w, Defs ops) {
    return w.sigma(DefVec(ops, [](const Def* op) { return op->unfold_type(); }));
}

const Def* Sigma::infer(World& w, Defs ops) {
    return w.umax<UMax::Kind>(DefVec(ops, [](const Def* op) { return op->unfold_type(); }));
}

const Def* Pi::infer(const Def* dom, const Def* codom) {
    auto& w = dom->world();
    return w.umax<UMax::Kind>({dom->unfold_type(), codom->unfold_type()});
}

const Def* Reform::infer(const Def* dom) { return dom->unfold_type(); }

/*
 * Def::check
 */

const Def* Def::check(size_t i, const Def* def) {
    auto lam = isa<Lam>();
    if (!lam) return def; // TODO Pi/Sigma/Arr/Rule accept any op for now

    if (i == 0) {
        if (auto filter = Checker::assignable(world().type_bool(), def)) return filter;
        Error(world().driver())
            .error(world().err_loc(def), "filter `{}` of lambda is of type `{}` but must be of type `Bool`", def,
                   type_of(def))
            .bail();
    }
    assert(i == 1);
    if (auto body = Checker::assignable(lam->codom(), def)) return body;
    Error(world().driver())
        .error(world().err_loc(def), "function body is not assignable to its declared codomain")
        .note("expected `{}`, got `{}`", lam->codom(), type_of(def))
        .note("body: `{}`", def)
        .note(lam->codom()->loc(), "codomain `{}` declared here", lam->codom())
        .bail();
}

const Def* Def::check() {
    auto& w = world();

    switch (node()) {
        case Node::Pi: {
            auto pi = as<Pi>();
            auto t  = Pi::infer(pi->dom(), pi->codom());
            if (!Checker::alpha<Checker::Check>(t, type()))
                w.error(w.err_loc(type()), "declared sort `{}` of function type does not match inferred sort `{}`",
                        type(), t);
            return t;
        }
        case Node::Arr: {
            auto t = as<Arr>()->body()->unfold_type();
            if (!Checker::alpha<Checker::Check>(t, type()))
                w.error(w.err_loc(type()), "declared sort `{}` of array does not match inferred sort `{}`", type(), t);
            return t;
        }
        case Node::Reform: {
            auto t = Reform::infer(as<Reform>()->dom());
            if (!Checker::alpha<Checker::Check>(t, type()))
                w.error(w.err_loc(type()), "declared sort `{}` of rule type does not match inferred sort `{}`", type(),
                        t);
            return t;
        }
        case Node::Sigma: {
            auto t = Sigma::infer(w, ops());
            if (t == type() || Checker::alpha<Checker::Check>(t, type())) return t; // TODO HACK
            w.log().w("incorrect type `{}` for `{}`; expected `{}` but keeping the existing type due to clos-conv "
                      "bugs",
                      type(), this, t);
            return type();
        }
        case Node::Rule: {
            auto rule = as<Rule>();
            auto t1   = rule->lhs()->unfold_type();
            auto t2   = rule->rhs()->unfold_type();
            if (!Checker::alpha<Checker::Check>(t1, t2))
                w.error(w.err_loc(type()), "type mismatch between rule sides: lhs has type `{}` but rhs has type `{}`",
                        t1, t2);
            if (!Checker::assignable(w.type_bool(), rule->guard()))
                w.error(w.err_loc(rule->guard()),
                        "condition `{}` of rewrite rule is of type `{}` but must be of type `Bool`", rule->guard(),
                        type_of(rule->guard()));
            return type();
        }
        default: return type();
    }
}

} // namespace mim
