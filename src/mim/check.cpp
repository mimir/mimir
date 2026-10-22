#include "mim/check.h"

#include <fe/assert.h>

#include "mim/rewrite.h"
#include "mim/rule.h"
#include "mim/world.h"

namespace mim {

bool Def::needs_zonk() const {
    if (has_hole()) {
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
template<Check::Mode mode>
bool Check::fail() {
    if (mode == C && world().flags().break_on_alpha) fe::breakpoint();
    return false;
}

const Def* Check::fail() {
    if (world().flags().break_on_alpha) fe::breakpoint();
    return {};
}
#endif

const Def* Check::is_uniform(Defs defs) {
    if (defs.empty()) return nullptr;
    auto first = defs.front();
    auto same  = [first](const Def* def) { return alpha<T>(first, def); };
    return std::ranges::all_of(defs.subspan(1), same) ? first : nullptr;
}

const Def* Check::coerce_(const Def* type, const Def* val) {
    auto val_t = val->unfold_type();
    if (!val_t) return fail(); // Level has no type, so it is assignable to nothing
    auto val_ty = val_t->zonk();
    if (type == val_ty) return val;

    auto& w = world();

    // Implicit insertion at a coercion site: @p val expects implicit arguments, but @p type is an
    // *explicit* function type. Fill them with Hole%s, as World::implicit_app does at application sites.
    // This lets polymorphic functions be passed as arguments, e.g. `affine.id` to a parameter of type
    // `«r; affine.Index» → «r; affine.Index»`, without writing `affine.id @r`.
    // Only do this when @p type is a Pi: if it is a Hole, we'd commit before knowing what's expected;
    // if it's an aggregate, we might consume implicits belonging to the value's element type.
    if (auto pi = type->isa<Pi>(); pi && !pi->is_implicit() && Pi::isa_implicit(val_ty)) {
        val    = w.implicit_holes(val);
        val_ty = val->unfold_type()->zonk();
        if (type == val_ty) return val;
    }

    if (auto sigma = type->isa<Sigma>()) {
        if (!alpha_<C>(type->arity(), val_ty->arity())) return fail();

        size_t a     = sigma->num_ops();
        auto new_ops = DefVec(a, [&](size_t i) { return val->proj(a, i); });
        for (size_t i = 0; i != a; ++i) {
            // A telescope's component only depends on its predecessors, which must have been checked.
            auto new_val = coerce_(w.component(sigma, new_ops, a, i), new_ops[i]);
            if (!new_val) return fail();
            new_ops[i] = new_val;
        }
        return w.tuple(new_ops);
    }

    // A dynamic Extract's type joins its components' types, which may only agree once each is coerced on its own.
    if (auto extract = val->isa<Extract>();
        extract && val_ty->isa<Join>() && !type->isa<Join>() && !Hole::isa_unset(type)) {
        auto tuple = extract->tuple();
        if (auto sigma = tuple->unfold_type()->zonk()->isa_imm<Sigma>()) {
            auto n       = sigma->num_ops();
            auto new_ops = DefVec(n);
            for (size_t i = 0; i != n; ++i)
                if (!(new_ops[i] = coerce_(type, tuple->proj(n, i)))) return fail();
            return w.extract(w.tuple(new_ops), extract->index());
        }
    }

    auto num_deferred = deferred_.size();
    if (alpha_<C>(type, val_ty)) return val;
    // Ex falso: a value of the empty type is dead code, e.g. an extract with an `Idx 0` index.
    if (auto bot = val_ty->isa<Bot>(); bot && bot->type()->isa<Type>()) {
        deferred_.resize(num_deferred);
        return val;
    }
    return fail();
}

std::pair<Check::Binders::iterator, bool> Check::bind(Def* mut, const Def* d) {
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

/// The statically known rank of @p def: `0` if it isn't a Seq at all, `nullopt` if its own rank is dynamic.
static std::optional<nat_t> known_rank(const Def* def) {
    if (auto seq = def->zonk_mut()->isa<Seq>()) return seq->shape().rank();
    return 0;
}

/// The rank of `«s; T»` with `s: «r; Nat»` is unknown as long as `r` is, and so is Def::arity.
/// @returns the unset Hole standing for `r`, or `nullptr`.
static Hole* isa_flex_rank(const Def* def) {
    if (auto seq = def->isa_imm<Seq>()) {
        if (auto shape = Hole::isa_unset(seq->shape()->zonk_mut())) {
            if (auto arr = shape->type()->zonk_mut()->isa<Arr>()) return Hole::isa_unset(arr->arity()->zonk_mut());
        }
    }
    return nullptr;
}

// These may be α-equivalent to a Def with a different Node or Def::flags(); see alpha_impl_.
static bool is_flex(const Def* def) {
    auto n = def->node();
    return n == Node::Hole || Prod::isa_node(n) || Seq::isa_node(n);
}

template<Check::Mode mode>
std::optional<bool> Check::try_alpha_(const Def* d1, const Def* d2) {
    // Pointer equality decides the matter, unless a free Var of an immutable is bound on one side only: λx.x vs λz.x.
    if (d1 == d2 && (d1->isa_mut() || bound_.empty() || !d1->has_free_vars_in(bound_))) return true;

    // Only a ground Def is stable under Def::zonk_mut, which rewires mutables in place and unifies Hole%s.
    if ((d1->node() != d2->node() || d1->flags() != d2->flags()) && d1->is_ground() && d2->is_ground() && !is_flex(d1)
        && !is_flex(d2))
        return fail<mode>();

    return {};
}

template<Check::Mode mode>
bool Check::alpha_(const Def* d1, const Def* d2) {
    if (auto res = try_alpha_<mode>(d1, d2)) return *res;

    auto& memo = memo_[mode];
    auto key   = memo_key(d1, d2);
    if (memo.contains(key)) return true;
    if (!alpha_impl_<mode>(d1, d2)) return false;
    memo.emplace(key);
    return true;
}

template<Check::Mode mode>
bool Check::alpha_impl_(const Def* d1, const Def* d2) {
    for (bool todo = true; todo;) {
        // below we check type and arity which may in turn open up more opportunities for zonking
        todo = false;
        d1   = d1->zonk_mut();
        d2   = d2->zonk_mut();

        if (auto res = try_alpha_<mode>(d1, d2); res.has_value()) return *res;

        auto h1 = d1->isa_mut<Hole>();
        auto h2 = d2->isa_mut<Hole>();

        if constexpr (mode == C) {
            if (h1) return check(h1, d2);
            if (h2) return check(h2, d1);
        } else if (h1 || h2) // mode == Test and h1 or h2 is an unresolved Hole
            return fail<T>();

        if (!d1->is_set() || !d2->is_set()) return fail<mode>();

        auto mut1 = d1->isa_mut();
        auto mut2 = d2->isa_mut();

        if (mut1 && mut2 && mut1 == mut2) return true;

        if (auto [i, ins] = bind(mut1, d2); !ins) return i->second == d2;
        if (auto [i, ins] = bind(mut2, d1); !ins) return i->second == d1;

        auto t1 = d1->type();
        auto t2 = d2->type();
        if (t1 && t2 && !alpha_<mode>(t1, t2)) return fail<mode>();

        // Def::arity of a flex-rank Seq is its first extent, which needs the rank pinned down first.
        if constexpr (mode == C) {
            // Only a statically known rank on the *other* side pins it down; two dynamic ranks unify
            // their shapes structurally further below.
            if (auto rank = isa_flex_rank(d1); rank && known_rank(d2)) {
                if (!check_rank(d1->as<Seq>(), rank, d2)) return fail<C>();
                todo = true;
                continue;
            }
            if (auto rank = isa_flex_rank(d2); rank && known_rank(d1)) {
                if (!check_rank(d2->as<Seq>(), rank, d1)) return fail<C>();
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

    if constexpr (mode == C) {
        if (auto lmax = d1->isa<LMax>()) return check(lmax, d2);
        if (auto lmax = d2->isa<LMax>()) return check(lmax, d1);
        if (auto inc = d1->isa<LSucc>()) return check(inc, d2);
        if (auto inc = d2->isa<LSucc>()) return check(inc, d1);

        if (seq1 && !seq1->shape().is_fused() && seq1->arity() == world().lit_nat_1() && !seq2) return check1(seq1, d2);
        if (seq2 && !seq2->shape().is_fused() && seq2->arity() == world().lit_nat_1() && !seq1) return check1(seq2, d1);

        if (seq1 && seq2) {
            if (auto mut_seq = seq1->isa_mut<Seq>(); mut_seq && seq2->isa_imm()) return check(mut_seq, seq2);
            if (auto mut_seq = seq2->isa_mut<Seq>(); mut_seq && seq1->isa_imm()) return check(mut_seq, seq1);
        }
    }

    if (auto prod = d1->isa<Prod>()) return check<mode>(prod, d2);
    if (auto prod = d2->isa<Prod>()) return check<mode>(prod, d1);
    if (seq1 && seq2) return check<mode>(seq1, seq2);

    if (d1->node() != d2->node() || d1->flags() != d2->flags()) return fail<mode>();

    if (auto var1 = d1->isa<Var>()) {
        auto var2 = d2->as<Var>();
        if (auto i = binders_.find(var1->binder()); i != binders_.end()) return i->second == var2->binder();
        if (binders_.contains(var2->binder())) return fail<mode>(); // var2 is bound
        // both var1 and var2 are free: OK, when they are the same or in Check mode
        return var1 == var2 || mode == C;
    }

    for (size_t i = 0, e = d1->num_ops(); i != e; ++i)
        if (!alpha_<mode>(d1->op(i), d2->op(i))) return fail<mode>();
    return true;
}

template<Check::Mode mode>
bool Check::check(const Prod* prod, const Def* def) {
    size_t a = prod->num_ops();
    for (size_t i = 0; i != a; ++i)
        if (!alpha_<mode>(prod->op(i), def->proj(a, i))) return fail<mode>();
    return true;
}

// A recursive type yields `l = max(ops..., l)` for its level whose least solution is `max(ops...)`.
static const Def* drop_self(Hole* hole, const Def* def) {
    auto lmax = def->isa<LMax>();
    if (!lmax) return def;

    DefVec ops;
    for (auto op : lmax->ops())
        if (op->zonk_mut() != hole) ops.emplace_back(op);

    return ops.size() == lmax->num_ops() ? def : hole->world().lmax(ops);
}

// A Hole may only be solved with a Def its type accepts; this is what pins `r` down in `s: «r; Nat»`.
bool Check::check(Hole* hole, const Def* def) {
    def = drop_self(hole, def);

    if (def->unfold_type()) { // Level has no type and is assignable to nothing
        if (auto new_def = coerce_(hole->type(), def))
            def = new_def;
        else
            return fail<C>();
    }
    return hole->set(def), true;
}

// alpha(«?s; body», «e₀, …, e_{r-1}; def»): the fused shape spells out every axis, so the rank follows from
// what `body` still claims - all of them while it is unknown, all but its own while it is a Seq itself.
bool Check::check_rank(const Seq* seq, Hole* rank, const Def* def) {
    auto r = known_rank(def);
    if (!r) return fail<C>();

    auto n    = *r;
    auto body = seq->body()->zonk_mut();
    if (!Hole::isa_unset(body))
        if (auto bseq = body->isa<Seq>())
            if (auto q = bseq->shape().rank(); q && *q <= *r) n = *r - *q;
    if (n == 0) return fail<C>();

    rank->set(world().lit_nat(n));
    Hole::isa_unset(seq->shape()->zonk_mut())->tuplefy(n);
    return true;
}

// alpha(«s₁; b₁», «s₂; b₂»): the two shapes may fuse a different number of axes, so compare the leading
// ones they share and the remainders - `«2, 3; T»` against `«2; X»` binds `X` to `«3; T»`.
template<Check::Mode mode>
bool Check::check(const Seq* seq1, const Seq* seq2) {
    auto r1 = seq1->shape().rank();
    auto r2 = seq2->shape().rank();
    if (r1 && r2 && *r1 != *r2) {
        auto k     = std::min(*r1, *r2);
        auto rest1 = world().drop(seq1, k);
        auto rest2 = world().drop(seq2, k);
        if (!rest1 || !rest2) return fail<mode>();
        for (size_t i = 0; i != k; ++i)
            if (!alpha_<mode>(seq1->shape()[i], seq2->shape()[i])) return fail<mode>();
        return alpha_<mode>(rest1, rest2);
    }

    return alpha_<mode>(*seq1->shape(), *seq2->shape()) && alpha_<mode>(seq1->body(), seq2->body());
}

// alpha(«1; body», def) -> alpha(body, def);
bool Check::check1(const Seq* seq, const Def* def) {
    auto body = seq->reduce(world().lit_idx_1_0()); // try to get rid of var inside of body
    if (!alpha_<C>(body, def)) return fail<C>();
    if (auto mut_seq = seq->isa_mut<Seq>()) mut_seq->set(world().lit_nat_1(), body->zonk());
    return true;
}

// Try to get rid of mut_seq's var: it may occur in its body and vanish after reduction
// as holes might have been filled in the meantime.
bool Check::check(Seq* mut_seq, const Seq* imm_seq) {
    // `mut_seq` binds only its own axes, so a *fused* `imm_seq` keeps the remaining ones in a sub-Seq:
    // `«i: n; Ts#i»` against `«n, c, h; T»` matches `Ts#⊤` with `«c, h; T»`, not with `T`.
    auto r    = mut_seq->shape().rank();
    auto rest = r ? world().drop(imm_seq, *r) : nullptr;

    if (!rest) return fail<C>();

    auto mut_body = mut_seq->reduce(world().top(world().type_indices(mut_seq->shape())));
    if (!alpha_<C>(mut_body, rest)) return fail<C>();

    mut_seq->set(*mut_seq->shape(), mut_body->zonk());
    return true;
}

// `max(xs, c) = l` determines the xs only if `l = 0` or a single x has to reach `l`; World::solve_levels does the rest.
bool Check::check(const LMax* lmax, const Def* def) {
    if (auto l = Lit::isa(def)) {
        auto c  = level_t(0);
        auto xs = DefVec();
        for (auto op : lmax->ops())
            if (auto k = Lit::isa(op))
                c = *k;
            else
                xs.emplace_back(op);

        if (c > *l) return fail<C>();
        if (*l == 0 || (c < *l && xs.size() == 1)) {
            for (auto x : xs)
                if (!alpha_<C>(x, def)) return fail<C>();
            return true;
        }
    }

    deferred_.emplace_back(lmax, def);
    return true;
}

// `x + k = l` determines `x = l - k`; with `l = y + j` the smaller offset cancels.
bool Check::check(const LSucc* inc, const Def* def) {
    auto k = inc->offset();
    if (auto l = Lit::isa(def)) return *l >= k && alpha_<C>(inc->op(), world().lit_level(*l - k));
    if (auto other = def->isa<LSucc>()) {
        auto j = other->offset();
        return j >= k ? alpha_<C>(inc->op(), world().lsucc(other->op(), j - k))
                      : alpha_<C>(world().lsucc(inc->op(), k - j), other->op());
    }
    return fail<C>();
}

void Check::commit() {
    for (auto [lmax, level] : deferred_)
        world().postpone(lmax, level);
}

#ifndef DOXYGEN
template bool Check::alpha_<Check::C>(const Def*, const Def*);
template bool Check::alpha_<Check::T>(const Def*, const Def*);
#endif

/*
 * infer
 */

const Def* Tuple::infer(World& w, Defs ops) {
    return w.sigma(DefVec(ops, [](const Def* op) { return op->unfold_type(); }));
}

const Def* Sigma::infer(World& w, Defs ops) { return w.type(w.level(ops)); }

const Def* Variant::infer(World& w, Defs ops) { return Sigma::infer(w, ops); }

const Def* Pi::infer(const Def* dom, const Def* codom) {
    auto& w = dom->world();
    return w.type(w.level({dom, codom}));
}

const Def* Reform::infer(const Def* dom) { return dom->unfold_type(); }

/*
 * Def::check
 */

const Def* Def::check(size_t i, const Def* def) {
    auto lam = isa<Lam>();
    if (!lam) return def; // TODO Pi/Sigma/Arr/Rule accept any op for now

    if (i == 0) {
        if (auto filter = Check::coerce(world().type_bool(), def)) return filter;
        def->blame("filter of a lambda is of type `{}` but must be of type `Bool`", type_of(def)).bail();
    }
    assert(i == 1);
    if (auto body = Check::coerce(lam->codom(), def)) return body;
    auto codom = lam->codom();
    auto& err  = def->blame("function body is not assignable to its declared codomain")
                     .n("expected `{}`, got `{}`", codom, type_of(def));
    if (codom->isa_mut() || codom->isa<Var>()) err.n(codom->loc(), "codomain `{}` declared here", codom);
    err.bail();
}

const Def* Def::check() {
    auto& w      = world();
    bool pending = type() == w.type_pending();

    switch (mut_node()) {
        case MutNode::Pi: {
            auto pi = as<Pi>();
            auto t  = Pi::infer(pi->dom(), pi->codom());
            if (!pending && !Check::alpha(t, type()))
                type()->blame("declared sort of function type does not match inferred sort `{}`", t).bail();
            return t;
        }
        case MutNode::Arr: {
            auto t = as<Arr>()->body()->unfold_type();
            if (!pending && !Check::alpha(t, type()))
                type()->blame("declared sort of array does not match inferred sort `{}`", t).bail();
            return t;
        }
        case MutNode::Sigma: {
            auto t = Sigma::infer(w, ops());
            if (pending || Check::alpha(t, type())) return t; // TODO HACK
            w.log().w("expected type {} for {} but keeping the declared {} due to clos-conv bugs", t, this, type());
            return type();
        }
        case MutNode::Variant: {
            auto t = Variant::infer(w, ops());
            if (!Check::alpha(t, type()))
                type()->blame("declared sort of variant does not match inferred sort `{}`", t).bail();
            return t;
        }
        case MutNode::Rule: {
            auto rule = as<Rule>();
            auto t1   = rule->lhs()->unfold_type();
            auto t2   = rule->rhs()->unfold_type();
            if (!Check::alpha(t1, t2))
                type()
                    ->blame("type mismatch between rule sides: lhs has type `{}` but rhs has type `{}`", t1, t2)
                    .bail();
            if (!Check::coerce(w.type_bool(), rule->guard()))
                rule->guard()
                    ->blame("condition of a rewrite rule is of type `{}` but must be of type `Bool`",
                            type_of(rule->guard()))
                    .bail();
            return type();
        }
        // A Lam's ops are checked one by one in Def::check(size_t, const Def*).
        case MutNode::Lam:
        case MutNode::Pack:
        case MutNode::Hole: return type();
    }
    std::unreachable();
}

} // namespace mim
