#include "mim/world.h"

#include <ranges>

#include <fe/container.h>
#include <fe/worklist.h>

#include "mim/check.h"
#include "mim/def.h"
#include "mim/driver.h"
#include "mim/rewrite.h"
#include "mim/schedule.h"
#include "mim/tuple.h"

#include "mim/util/gid.h"

namespace mim {

namespace {

/// Does `d#index \u2190 val` rebuild an aggregate right away? Mirrors the two folding rules in World::insert.
bool insert_rebuilds(const Def* d, const Def* index, u64 threshold) {
    if (!Lit::isa(index)) return false;
    if (d->isa<Tuple>()) return true;
    if (auto pack = d->isa<Pack>(); pack && !pack->shape().is_fused())
        if (auto a = Lit::isa(pack->arity())) return *a < threshold;
    return false;
}

/// How many axes @p type indexes in one go; `1` for anything that isn't an Arr, `std::nullopt` for a dynamic rank.
std::optional<nat_t> own_rank(const Def* type) {
    if (auto arr = type->isa<Arr>()) return arr->shape().rank();
    return 1;
}

/// `(t#is)#js` as *one* index into @p t; `nullptr` unless @p t's own shape has room for both ranks.
/// The Extract and the Insert rule are duals and share this check.
Shape fused_index(const Def* t, Shape is, Shape js) {
    auto arr = t->unfold_type()->isa<Arr>();
    if (!arr) return {};
    auto ri = is.rank();
    auto rj = js.rank();
    auto rt = arr->shape().rank();
    if (!ri || !rj || !rt || *ri + *rj > *rt) return {};
    return is + js;
}

/// Coerces @p val to @p elem_type or bails out with the "not assignable" diagnostic.
const Def* assign_or_bail(const Def* elem_type, const Def* val) {
    auto res = Checker::assignable(elem_type, val);
    if (!res)
        val->blame("value is not assignable to element type")
            .n("expected `{}`, got `{}`", elem_type, type_of(val))
            .n("value: `{}`", val)
            .bail();
    return res;
}

/// Checks the multi-dimensional @p index against @p d and yields the type it peels to.
const Def* nary_elem_type(const Def* d, Shape index) {
    auto type = d->unfold_type();
    auto arr  = type->isa<Arr>();
    if (!arr) index->blame("multi-dimensional index `{}` expects an array but got `{}`", *index, type).bail();

    auto shape = arr->shape();
    auto ri    = index.rank();
    auto rt    = shape.rank();
    // A *partial* index peels only its own axes and yields a sub-array.
    auto fit = ri && rt ? *ri <= *rt : Checker::alpha<Checker::Check>(index->arity(), shape->arity());
    if (!fit)
        index
            ->blame("index `{}` of rank `{}` does not fit shape `{}` of rank `{}`", *index, index->arity(), *shape,
                    shape->arity())
            .bail();

    // Only the axes World::extract folds away reach World::extract1's check, so the ones staying fused
    // need it here. A fused shape never binds the Arr's own var, so its extents compare as they stand.
    if (ri && rt)
        for (nat_t i = 0; i != *ri; ++i)
            if (auto size = Idx::isa(index[i]->unfold_type()); size && !Checker::alpha<Checker::Check>(shape[i], size))
                index[i]->blame("index `{}` does not fit within arity `{}`", index[i], shape[i]).bail();

    return d->world().peel(arr, *index);
}

/// A fresh mutable Seq of @p seq's kind holding @p elem at @p shape, whose body @p rest computes from its own var.
const Def* mut_shaped(const Seq* seq, Shape shape, const Def* elem, auto rest) {
    auto& w      = seq->world();
    auto is_pack = seq->is_intro();
    auto res     = w.mut_seq(is_pack, is_pack ? w.arr(shape, elem) : seq->type());
    res->Def::set(0, *shape);
    return res->Def::set(1, rest(res))->zonk_mut();
}

/// Marks a World::Reduct slot while it is being computed: seeing it again means the op requires its own reduction.
const Def* const Filling = (const Def*)1;

/// Sorts by gid and drops duplicates; Def%s are hash-consed, so pointer identity *is* structural identity.
void sort_unique(DefVec& defs) {
    std::ranges::sort(defs, GIDLt<const Def*>());
    defs.erase(std::unique(defs.begin(), defs.end()), defs.end());
}

} // namespace

void World::Externals::externalize(Def* def) {
    assert(!def->is_external());
    assert(def->is_closed());
    def->external_ = true;
    fe::assert_emplace(sym2mut_, def->sym(), def);
}

void World::Externals::internalize(Def* def) {
    assert(def->is_external());
    def->external_ = false;
    auto num       = sym2mut_.erase(def->sym());
    assert_unused(num == 1);
}

const Def* World::Annexes::attach(flags_t flags, Sym sym, const Def* def) {
    driver().log().t("register annex `{}` 0x{:x} → {}", sym, flags, def);
    if (driver().is_loaded(Annex::demangle(flags))) {
        fe::assert_emplace(flags2entry_, flags, Annexes::Entry{sym, def});
        fe::assert_emplace(sym2flags_, sym, flags);
        def->annex_ = true;
        return def;
    }
    return nullptr;
}

void World::Annexes::attach_alias(flags_t flags, Sym sym) {
    if (!driver().is_loaded(Annex::demangle(flags))) return;
    // An alias spelled the same as its target's own (unqualified) name registers the identical
    // qualified string as the target - a benign no-op, not a conflict.
    if (auto [i, ins] = sym2flags_.try_emplace(sym, flags); !ins) assert(i->second == flags);
}

/*
 * constructor & destructor
 */

#if (!defined(_MSC_VER) && defined(NDEBUG))
bool World::Lock::guard_ = false;
#endif

World::World(Driver* driver, const State& state)
    : driver_(driver)
    , zonker_(*this)
    , state_(state)
    , move_(driver) {
    data_.univ        = insert<Univ>(*this);
    data_.lit_univ_0  = lit_univ(0);
    data_.lit_univ_1  = lit_univ(1);
    data_.type_0      = type(lit_univ_0());
    data_.type_1      = type(lit_univ_1());
    data_.type_bot    = insert<Bot>(type());
    data_.type_top    = insert<Top>(type());
    data_.sigma       = unify<Sigma>(type(), Defs{})->as<Sigma>();
    data_.tuple       = unify<Tuple>(sigma(), Defs{})->as<Tuple>();
    data_.type_nat    = insert<mim::Nat>(*this);
    data_.type_idx    = insert<mim::Idx>(pi(type_nat(), type()));
    data_.top_nat     = insert<Top>(type_nat());
    data_.lit_nat_0   = lit_nat(0);
    data_.lit_nat_1   = lit_nat(1);
    data_.lit_idx_1_0 = lit_idx(1, 0);
    data_.type_bool   = type_idx(2);
    data_.lit_bool[0] = lit_idx(2, 0_u64);
    data_.lit_bool[1] = lit_idx(2, 1_u64);
    data_.lit_nat_max = lit_nat(nat_t(-1));
}

World::World(Driver* driver, Sym name)
    : World(driver, State(name)) {}

// ~Def() has nothing to do, so World does not run it.
World::~World() = default;

static_assert(std::is_trivially_destructible_v<Dbg> && std::is_trivially_destructible_v<Vars>
                  && std::is_trivially_destructible_v<Muts> && std::is_trivially_destructible_v<NormalizeFn>,
              "a Def member gained a non-trivial destructor: World::~World must destroy Defs again");

/*
 * Driver
 */

fe::Error& World::error() { return driver().error(); }
const fe::Error& World::error() const { return driver().error(); }
const fe::Log& World::log() const { return driver().log(); }
Flags& World::flags() { return driver().flags(); }

Sym World::sym(const char* s) { return driver().sym(s); }
Sym World::sym(std::string_view s) { return driver().sym(s); }
Sym World::sym(const std::string& s) { return driver().sym(s); }

/*
 * factory methods
 */

const Type* World::type(const Def* level) {
    if (!level) return nullptr;
    level = level->zonk();

    if (!level->isa_type<Univ>())
        level->blame("argument `{}` to `Type` must be of type `Univ` but is of type `{}`", level, type_of(level))
            .bail();

    return unify<Type>(level)->as<Type>();
}

const Def* World::uinc(const Def* op, level_t offset) {
    op = op->zonk();

    if (!op->isa_type<Univ>())
        op->blame("operand `{}` of a universe increment must be of type `Univ` but is of type `{}`", op, type_of(op))
            .bail();

    if (auto l = Lit::isa(op)) return lit_univ(*l + 1);
    return unify<UInc>(op, offset);
}

static void flatten_umax(DefVec& ops, const Def* def) {
    if (auto umax = def->isa<UMax>())
        for (auto op : umax->ops())
            flatten_umax(ops, op);
    else
        ops.emplace_back(def);
}

template<int sort>
const Def* World::umax(Defs ops_) {
    DefVec ops;
    ops.reserve(ops_.size());
    for (auto op : ops_) {
        op = op->zonk();

        // Peel off as many layers as the sort of the incoming ops demands to arrive at a Univ level:
        // a Univ level is already there, a Kind is a `Type lvl`, a Type needs one unfold, a term two.
        if constexpr (sort >= UMax::Type) op = op->unfold_type();
        if constexpr (sort == UMax::Term) op = op->unfold_type();
        if constexpr (sort >= UMax::Kind) {
            if (auto type = op->isa<Type>())
                op = type->level();
            else
                op->blame("operand `{}` must be a `Type` of some universe level", op).bail();
        }

        flatten_umax(ops, op);
    }

    level_t lvl = 0;
    DefVec res;
    res.reserve(ops.size());
    for (auto op : ops) {
        if (!op->isa_type<Univ>())
            op->blame("operand `{}` of a universe max must be of type `Univ` but is of type `{}`", op, type_of(op))
                .bail();

        if (auto l = Lit::isa(op))
            lvl = std::max(lvl, *l);
        else
            res.emplace_back(op);
    }

    const Def* l = lit_univ(lvl);
    if (res.empty()) return sort == UMax::Univ ? l : type(l);
    if (lvl > 0) res.emplace_back(l);

    sort_unique(res);
    const Def* umax = unify<UMax>(*this, res);
    return sort == UMax::Univ ? umax : type(umax);
}

// TODO more thorough & consistent checks for singleton types

const Def* World::var(Def* mut) {
    if (auto var = mut->var_) return var;

    if (auto var_type = mut->var_type()) { // could be nullptr, if frozen
        if (auto s = Idx::isa(var_type)) {
            if (auto l = Lit::isa(s); l && l == 1) return lit_idx_1_0();
        } else if (auto s = var_type->isa<Sigma>(); s && s->num_ops() == 0)
            return tuple(s, {});
    }

    return mut->var_ = unify<Var>(mut);
}

template<bool Normalize>
const Def* World::implicit_app(const Def* callee, const Def* arg) {
    while (auto pi = Pi::isa_implicit(callee->unfold_type()))
        callee = app(callee, mut_hole(pi->dom()));
    return app<Normalize>(callee, arg);
}

template<bool Normalize>
const Def* World::app(const Def* callee, const Def* arg) {
    callee = callee->zonk();
    arg    = arg->zonk();

    auto pi = callee->isa_type<Pi>();
    if (!pi)
        callee->blame("callee is not of function type")
            .n("callee `{}` has type `{}`", callee, type_of(callee))
            .n(callee->loc(), "callee `{}` declared here", callee)
            .bail();

    auto new_arg = Checker::assignable(pi->dom(), arg);
    if (!new_arg)
        arg->blame("argument is not assignable to callee's domain")
            .n("expected `{}`, got `{}`", pi->dom(), type_of(arg))
            .n(callee->loc(), "callee `{}` declared here", callee)
            .bail();

    // re-zonk after assignable check above - we might have inferred new stuff
    arg    = new_arg->zonk();
    callee = callee->zonk();
    pi     = callee->isa_type<Pi>();

    // always β-reduce non-recursive, non-parametric lambdas
    if (auto imm = callee->isa_imm<Lam>()) return imm->body();

    if (auto lam = callee->isa_mut<Lam>(); lam && lam->is_set()) {
        auto var = lam->has_var();

        // Applying a Lam to its own Var is the identity substitution, so it resolves to the body.
        // This unfolds a self-application / fixed-point reference.
        if (var && arg == var) return lam->body();

        // β-reduce or partially evaluate a set, mutable Lam.
        if (lam->filter() != lit_ff()) {
            if (!var) {
                if (lam->filter() == lit_tt()) return lam->body();
            } else if (auto i = move_.substs.find({var, arg}); i != move_.substs.end()) {
                // Reuse the cached reduct if its filter held.
                auto [filter, body] = i->second->ops<2>();
                if (filter == lit_tt()) return body;
            } else {
                // Evaluate the filter; if it holds, reduce the body and cache the reduct.
                auto rw     = VarRewriter(var, arg);
                auto filter = rw.rewrite(lam->filter());
                if (filter == lit_tt()) {
                    log().d("partially evaluate {} ({})", lam, arg);
                    auto body = rw.rewrite(lam->body());
                    cache_reduct(var, arg, {filter, body});
                    return body;
                }
            }
        }
    }

    auto type               = pi->reduce(arg)->zonk();
    callee                  = callee->zonk();
    auto [axm, curry, trip] = Axm::next(callee);

    if (axm)
        if (auto normalizer = axm->normalizer(); Normalize && normalizer && curry == 0)
            if (auto norm = normalizer(type, callee, arg)) return norm;

    return raw_app(axm, curry, trip, type, callee, arg);
}

const Def* World::raw_app(const Def* type, const Def* callee, const Def* arg) {
    type   = type->zonk();
    callee = callee->zonk();
    arg    = arg->zonk();

    auto [axm, curry, trip] = Axm::next(callee);
    return raw_app(axm, curry, trip, type, callee, arg);
}

const Def* World::raw_app(const Axm* axm, u8 curry, u8 trip, const Def* type, const Def* callee, const Def* arg) {
    return unify<App>(axm, curry, trip, type, callee, arg);
}

const Def* World::sigma(Defs ops) {
    auto n = ops.size();
    if (n == 0) return sigma();
    if (n == 1) return ops[0]->zonk();

    auto zops = Def::zonk(ops);
    if (auto uni = Checker::is_uniform(zops)) return arr(n, uni);
    return unify<Sigma>(Sigma::infer(*this, zops), zops);
}

const Def* World::tuple(Defs ops) {
    auto n = ops.size();
    if (n == 0) return tuple();
    if (n == 1) return ops[0]->zonk();

    auto zops  = Def::zonk(ops);
    auto sigma = Tuple::infer(*this, zops);
    auto t     = tuple(sigma, zops);
    auto new_t = Checker::assignable(sigma, t);
    if (!new_t)
        t->blame("tuple `{}` of type `{}` is not assignable to inferred type `{}`", t, type_of(t), sigma).bail();

    return new_t;
}

const Def* World::tuple(const Def* type, Defs ops_) {
    // TODO type-check type vs inferred type
    type     = type->zonk();
    auto ops = Def::zonk(ops_);

    auto n = ops.size();
    if (!type->isa_mut<Sigma>()) {
        if (n == 0) return tuple();
        if (n == 1) return ops[0];
        if (auto uni = Checker::is_uniform(ops)) return pack(n, uni);
    }

    // eta rule for tuples: (extract(tup, 0), extract(tup, 1), extract(tup, 2)) -> tup
    if (auto ex0 = n != 0 ? ops[0]->isa<Extract>() : nullptr) {
        auto tup = ex0->tuple();
        bool eta = tup->type() == type;
        for (size_t i = 0; i != n && eta; ++i) {
            auto ex = ops[i]->isa<Extract>();
            auto id = ex ? Lit::isa(ex->index()) : std::nullopt;
            eta     = ex && id && *id == u64(i) && ex->tuple() == tup;
        }

        if (eta) return tup;
    }

    return unify<Tuple>(type, ops);
}

const Def* World::tuple(Sym sym) {
    return tuple(DefVec(sym, [this](char c) { return lit_i8(c); }));
}

const Def* World::extract(const Def* d, const Def* index_) {
    if (!d || !index_) return nullptr; // can happen if frozen
    d = d->zonk();

    auto index = check_index(index_->zonk());
    if (index.is_dim()) return extract1(d, *index);

    auto r = index.rank();
    if (!r) return extract_fused(d, index); // a dynamic rank has no axes to fold one by one
    if (*r == 0) return d;                  // the index folded away entirely, i.e. `d` is its own only element

    // Fold from the left as far as `d` allows; whatever is left stays *fused*, mirroring `«a, b; T»`.
    for (nat_t i = 0; i != *r; ++i) {
        auto c    = index[i];
        auto next = extract1(d, c);
        if (auto ex = next->isa<Extract>(); ex && ex->tuple() == d && ex->index() == c)
            return extract_fused(d, index.drop(i));
        d = next;
    }

    return d;
}

/// Validates @p index and folds its literal size-1 axes away, mirroring the ones World::seq folds out of a shape:
/// `d#(i, 0₁, k)` ≡ `d#(i, k)`. A *scalar* index is left alone - its `Idx 1` carve-out for mutable 1-tuples still
/// applies - and so is a dynamic rank, which has no axes to inspect.
Shape World::check_index(const Def* index) {
    auto type = index->unfold_type();
    if (Idx::isa(type)) return index;
    if (!Shape::isa_indices(type))
        index->blame("index `{}` must be of `Idx` type but is of type `{}`", index, type_of(index)).bail();
    return Shape(index).fold();
}

const Def* World::drop(const Seq* s, nat_t k) {
    auto shape = s->shape();
    auto r     = shape.rank();
    if (!r || k > *r || s->has_var()) return nullptr; // a dependent shape needs an index; cf. World::peel
    if (k == *r) return s->body();
    if (is_frozen()) return nullptr; // splitting a fused shape builds the sub-Seq
    return seq(s->is_intro(), shape.drop(k), s->body());
}

const Def* World::peel(const Seq* seq, const Def* index) {
    if (!index) return nullptr; // can happen if frozen
    auto shape = seq->shape();
    auto rank  = shape.rank();
    auto k     = Shape(index).rank();
    if (!rank || !k || *k >= *rank) return seq->reduce(index); // the index covers every axis
    if (!seq->has_var()) return drop(seq, *k);
    if (is_frozen()) return nullptr; // splitting a fused shape builds the sub-Seq

    // Dependent: the leading axes come from @p index, the trailing ones from the result's own index.
    return mut_shaped(seq, shape.drop(*k), seq->body()->unfold_type(),
                      [&](Seq* res) { return seq->reduce(*(Shape(index) + res->var())); });
}

const Def* World::fuse(Seq* seq) {
    auto outer = seq->shape();
    auto inner = seq->body() ? seq->body()->zonk()->isa<Seq>() : nullptr;
    auto var   = seq->has_var();
    // A ragged nest - one whose inner extents depend on the outer index - has no fused shape to spell them in.
    if (!inner || inner->node() != seq->node() || (var && inner->shape()->has_free_var(var))) return seq->zonk_mut();

    auto ro    = outer.rank();
    auto shape = outer + inner->shape();
    if (!ro || !shape) return seq->zonk_mut();

    // The fused index supplies both levels: its leading `ro` axes replace the outer one, the rest the inner.
    return mut_shaped(seq, shape, inner->body()->unfold_type(), [&](Seq* res) {
        auto v = Shape(res->var());
        return seq->reduce(*v.take(*ro))->as<Seq>()->reduce(*v.drop(*ro));
    });
}

const Def* World::type_indices(Shape shape) {
    if (shape.is_dim()) return type_idx(*shape);
    if (auto r = shape.rank()) return sigma(shape->projs(*r, [this](const Def* e) { return type_idx(e); }));

    // `«i: r; Idx (s#i)»` - a dynamic rank cannot spell out its axes.
    auto res = mut_arr(type())->set_shape(shape->arity());
    return res->set_body(type_idx(extract(*shape, res->var())))->zonk_mut();
}

/// Builds `d#index` for a multi-dimensional @p index that does not fold any further.
const Def* World::extract_fused(const Def* d, Shape index) {
    if (index.is_dim()) return extract1(d, *index);

    // An index reaching past `d`'s own shape continues through the element type - `«2; [Nat, Bool]»` is rank 1,
    // yet `t#(i, j)` is still `t#i#j`. Split it at the boundary, so each Extract matches the shape it indexes.
    auto ri = index.rank();
    auto rt = own_rank(d->unfold_type());
    if (ri && rt && *ri > *rt && !is_frozen()) return extract(extract_fused(d, index.take(*rt)), *index.drop(*rt));

    // `d#is ← val` then `#is` -> `val`; cf. the same rule in World::extract1
    if (auto insert = d->isa<Insert>(); insert && insert->index() == *index) return insert->value();
    return unify<Extract>(nary_elem_type(d, index), d, *index);
}

/// Type-checks a *scalar* @p index of `Idx size` against @p type.
/// @returns `true` if the axis has folded out of @p type - `«1; T»` ≡ `T` - so that `d#index` *is* `d` and
/// `d#index ← val` replaces all of it.
bool World::is_folded_axis(const Def* type, const Def* index, const Def* size) {
    if (Lit::isa(size) == 1) {
        if (Lit::isa(index) != 0) log().w("index of `Idx 1` is not the literal 0: {}", index);
        // A *mutable* Sigma may be a genuine 1-tuple and still needs a real Extract/Insert; TODO mutable Arr?
        auto sigma = type->isa_mut<Sigma>();
        if (!sigma || sigma->num_ops() != 1) return true;
    }

    if (!Checker::alpha<Checker::Check>(type->arity(), size))
        index->blame("index `{}` does not fit within arity `{}`", index, type->arity()).bail();
    return false;
}

const Def* World::extract1(const Def* d, const Def* index) {
    auto index_ty = index->unfold_type();
    auto size     = Idx::isa(index_ty);
    auto lidx     = Lit::isa(index);
    auto type     = d->unfold_type();

    if (is_folded_axis(type, index, size)) return d;

    if (auto pack = d->isa<Pack>()) {
        if (pack->shape().is_fused()) return peel(pack, index); // one axis off a fused Pack yields a sub-Pack
        if (pack->has_var())
            return pack->reduce(index);
        else
            return pack->body();
    }

    // extract(insert(x, index, val), index) -> val
    if (auto insert = d->isa<Insert>()) {
        if (index == insert->index()) return insert->value();
    }

    // `(t#is)#j` -> `t#(is, j)`: the maximal-rank Extract into one fused Arr is the normal form.
    if (auto ex = is_frozen() ? nullptr : d->isa<Extract>())
        if (auto is = fused_index(ex->tuple(), ex->index(), index)) return extract_fused(ex->tuple(), is);

    if (lidx) {
        // A Hole only tuplefies into a known number of components; a dynamic rank leaves a symbolic Extract.
        if (auto hole = d->isa_mut<Hole>())
            if (auto n = Idx::isa_lit(index_ty)) d = hole->tuplefy(*n);
        if (auto tuple = d->isa<Tuple>()) return tuple->op(*lidx);

        // extract(insert(x, j, val), i) -> extract(x, i) where i != j (guaranteed by rule above)
        if (auto insert = d->isa<Insert>()) {
            if (insert->index()->isa<Lit>()) return extract(insert->tuple(), index);
        }

        if (auto sigma = type->isa<Sigma>()) {
            if (auto var = sigma->has_var()) {
                if (d == var) return unify<Extract>(sigma->op(*lidx), d, index); // `var -> var` is the identity
                // Frozen, only an already cached reduct can be replayed - rewriting would create nodes.
                if (is_frozen()) {
                    auto t = cached_reduct(var, d, *lidx);
                    return t ? unify<Extract>(t, d, index) : nullptr;
                }
                return unify<Extract>(reduce(var, d, *lidx), d, index);
            }

            return unify<Extract>(sigma->op(*lidx), d, index);
        }
    }

    const Def* elem_t;
    if (auto arr = type->isa<Arr>()) {
        elem_t = peel(arr, index);
    } else {
        auto sigma = type->as<Sigma>();
        elem_t     = nullptr;
        // «(a_0, ..., a_{n-1})#index; body» is more precise than the join if all ops are Arrs of the same body.
        if (sigma->isa_imm()) {
            const Def* body = nullptr;
            auto extents    = DefVec();
            for (auto op : sigma->ops()) {
                auto op_arr = op->zonk()->isa<Arr>();
                if (!op_arr || (body && op_arr->body()->zonk() != body)) {
                    extents.clear();
                    break;
                }
                body = op_arr->body()->zonk();
                extents.emplace_back(*op_arr->shape());
            }
            if (!extents.empty()) elem_t = this->arr(extract(tuple(extents), index), body);
        }
        if (!elem_t) elem_t = join(sigma->ops());
    }

    if (index->isa<Top>()) {
        if (auto hole = Hole::isa_unset(d)) {
            auto elem_hole = mut_hole(elem_t);
            hole->set(pack(size, elem_hole));
            return elem_hole;
        }
    }

    assert(d);
    return unify<Extract>(elem_t, d, index);
}

const Def* World::insert(const Def* d, const Def* index_, const Def* val) {
    d   = d->zonk();
    val = val->zonk();

    auto type  = d->unfold_type();
    auto index = check_index(index_->zonk());

    // `d#(i, j, k) ← val` stays *fused* unless the outermost write rebuilds an aggregate on the spot.
    if (!index.is_dim()) {
        auto r = index.rank();
        // The index folded away entirely, so the write replaces all of `d`.
        if (r && *r == 0) return assign_or_bail(type, val);

        // A write deeper than `d`'s own shape reads down to the boundary and writes the levels back out;
        // cf. World::extract_fused. So does one whose outermost write rebuilds an aggregate.
        auto rt  = own_rank(type);
        auto cut = r && rt && *r > *rt                                            ? rt
                 : r && insert_rebuilds(d, index[0], flags().scalarize_threshold) ? std::optional<nat_t>(1)
                                                                                  : std::nullopt;
        if (cut) {
            auto head = *index.take(*cut);
            return insert(d, head, insert(extract(d, head), *index.drop(*cut), val));
        }

        auto new_val = assign_or_bail(nary_elem_type(d, index), val);

        // `d#is ← (d#is)#js ← val` -> `d#(is, js) ← val`; cf. the scalar path below, which reaches this
        // through World::extract's per-axis loop - the fused path has none, so it needs the rule itself.
        if (auto inner = is_frozen() ? nullptr : new_val->isa<Insert>())
            if (auto ex = inner->tuple()->isa<Extract>(); ex && ex->tuple() == d && ex->index() == *index)
                if (auto is = fused_index(d, *index, inner->index())) return insert(d, *is, inner->value());

        // `d#is ← d#is` -> `d` and `(d#is ← y)#is ← val` -> `d#is ← val`; cf. the scalar path below
        if (auto ex = new_val->isa<Extract>(); ex && ex->tuple() == d && ex->index() == *index) return d;
        if (auto insert = d->isa<Insert>(); insert && insert->index() == *index) d = insert->tuple();
        return unify<Insert>(d, *index, new_val);
    }

    auto size = Idx::isa(index->unfold_type());
    auto lidx = Lit::isa(*index);

    if (is_folded_axis(type, *index, size)) return assign_or_bail(type, val); // the write replaces all of `d`
    // A Sigma needs a literal index to name the component; an Arr peels to an element type at any index.
    if (auto arr = type->isa<Arr>()) {
        if (auto elem = peel(arr, *index)) val = assign_or_bail(elem, val);
    } else if (lidx) {
        val = assign_or_bail(type->proj(Lit::as(size), *lidx), val);
    }
    // The only `Idx 1` left is a mutable 1-tuple; d could be mut - that's why the tuple ctor is needed.
    if (Lit::isa(size) == 1) return tuple(d, {val});

    // insert((a, b, c, d), 2, x) -> (a, b, x, d)
    if (auto t = d->isa<Tuple>(); t && lidx) {
        auto new_ops   = DefVec(t->ops().begin(), t->ops().end());
        new_ops[*lidx] = val;
        return tuple(type, new_ops);
    }

    // insert(‹4; x›, 2, y) -> (x, x, y, x)
    if (auto pack = d->isa<Pack>(); pack && lidx) {
        if (auto a = Lit::isa(pack->arity()); a && *a < flags().scalarize_threshold) {
            auto new_ops   = DefVec(*a, pack->body());
            new_ops[*lidx] = val;
            return tuple(type, new_ops);
        }
    }

    // `d#i ← (d#i)#js ← val` -> `d#(i, js) ← val`: the dual of the Extract fusion in World::extract1, and what
    // keeps the read-modify-write chain a *single* write. The aggregate-rebuilding rules above win over it.
    if (auto inner = is_frozen() ? nullptr : val->isa<Insert>())
        if (auto ex = inner->tuple()->isa<Extract>(); ex && ex->tuple() == d && ex->index() == *index)
            if (auto is = fused_index(d, *index, inner->index())) return insert(d, *is, inner->value());

    // insert(d, index, d#index) -> d
    if (auto ex = val->isa<Extract>())
        if (ex->tuple() == d && ex->index() == *index) return d;

    // insert(insert(x, index, y), index, val) -> insert(x, index, val)
    if (auto insert = d->isa<Insert>()) {
        if (insert->index() == *index) d = insert->tuple();
    }

    return unify<Insert>(d, *index, val);
}

const Def* World::seq(bool is_pack, Shape shape, const Def* body) {
    shape = shape.zonk();
    body  = body->zonk();

    auto shape_ty = shape->unfold_type();
    if (!Shape::isa_extents(shape_ty))
        shape->blame("expected shape but got `{}` of type `{}`", *shape, shape_ty).bail();

    // `«1; T»` ≡ `T`, mirroring `[T]` ≡ `T`, so a literal size-1 axis folds out of the shape.
    shape  = shape.fold();
    auto r = shape.rank();
    if (r) {
        if (*r == 0) return body;
        // A literal-`0` extent empties everything below it: `«2, 0, 3; T»` is `«2; []»`, not `[]`.
        for (nat_t i = 0; i != *r; ++i)
            if (Shape::extent(shape[i]) == 0) return seq(is_pack, shape.take(i), unit(is_pack));
    }

    // `«a; «b; T»»` ≡ `«a, b; T»` - the very compression that already makes `«3; T»` out of `[T, T, T]`,
    // one level up. A *mutable* body binds an index the fused shape could not express, so it stops fusion.
    if (auto inner = body->isa_imm<Seq>(); inner && inner->is_intro() == is_pack)
        if (auto fused = shape + inner->shape()) return seq(is_pack, fused, inner->body());

    if (is_pack) return unify<Pack>(arr(shape, body->unfold_type()), *shape, body);
    return unify<Arr>(body->unfold_type(), *shape, body);
}

const Def* World::seq(bool is_pack, Defs shape, const Def* body) {
    if (shape.empty()) return body;
    return seq(is_pack, tuple(shape), body);
}

const Lit* World::lit(const Def* type, u64 val) {
    if (!type) return nullptr;
    type = type->zonk();

    if (auto size = Idx::isa(type)) {
        if (size->isa<Top>()) {
            // unsafe but fine
        } else if (auto s = Lit::isa(size)) {
            if (*s != 0 && val >= *s) type->blame("index `{}` does not fit within arity `{}`", val, size).bail();
        } else if (val != 0) { // 0 of any size is allowed
            type->blame("cannot create literal `{}` of `Idx {}` as size is unknown", val, size).bail();
        }
    }

    return unify<Lit>(type, val);
}

/*
 * set
 */

template<bool Up>
const Def* World::ext(const Def* type) {
    type = type->zonk();

    if (auto arr = type->isa<Arr>()) return pack(arr->shape(), ext<Up>(arr->body()));
    if (auto sigma = type->isa<Sigma>())
        return tuple(sigma, DefVec(sigma->ops(), [this](const Def* op) { return ext<Up>(op); }));
    return unify<TExt<Up>>(type);
}

template<bool Up>
const Def* World::bound(Defs ops_) {
    auto ops = DefVec();
    ops.reserve(ops_.size());
    for (auto op_ : ops_) {
        auto op = op_->zonk();
        if (!op->isa<TExt<!Up>>()) ops.emplace_back(op); // ignore: ext<!Up>
    }

    auto kind = umax<UMax::Type>(ops);

    // has ext<Up> value?
    if (std::ranges::any_of(ops, [](const Def* op) { return op->isa<TExt<Up>>(); })) return ext<Up>(kind);

    sort_unique(ops);

    if (ops.empty()) return ext<!Up>(kind);
    if (ops.size() == 1) return ops[0];

    // TODO simplify mixed terms with joins and meets?
    return unify<TBound<Up>>(kind, ops);
}

const Def* World::merge(const Def* type, Defs ops_) {
    type     = type->zonk();
    auto ops = Def::zonk(ops_);

    if (type->isa<Meet>()) {
        auto types = DefVec(ops.size(), [&](size_t i) { return ops[i]->unfold_type(); });
        return unify<Merge>(meet(types), ops);
    }

    assert(ops.size() == 1);
    return ops[0];
}

const Def* World::merge(Defs ops_) {
    auto ops = Def::zonk(ops_);
    return merge(umax<UMax::Term>(ops), ops);
}

const Def* World::inj(const Def* type, const Def* value) {
    type  = type->zonk();
    value = value->zonk();

    if (type->isa<Join>()) return unify<Inj>(type, value);
    return value;
}

const Def* World::split(const Def* type, const Def* value) {
    type  = type->zonk();
    value = value->zonk();

    return unify<Split>(type, value);
}

const Def* World::match(Defs ops_) {
    auto ops = Def::zonk(ops_);
    if (ops.size() == 1) return ops.front();

    auto scrutinee = ops.front();
    auto arms      = ops.span().subspan(1);
    auto join      = scrutinee->isa_type<Join>();

    if (!join)
        scrutinee
            ->blame("scrutinee `{}` of a test expression must be of union type but has type `{}`", scrutinee,
                    type_of(scrutinee))
            .bail();

    if (arms.size() != join->num_ops())
        scrutinee->blame("test expression has {} arms but union type has {} cases", arms.size(), join->num_ops())
            .bail();

    for (auto arm : arms)
        if (!arm->isa_type<Pi>())
            arm->blame("arm `{}` of test expression does not have a function type but has type `{}`", arm, type_of(arm))
                .bail();

    std::ranges::sort(arms, GIDLt<const Def*>(), [](const Def* arm) { return arm->isa_type<Pi>()->dom(); });

    const Def* type = nullptr;
    for (size_t i = 0, e = arms.size(); i != e; ++i) {
        auto arm = arms[i];
        auto pi  = arm->isa_type<Pi>();
        if (!Checker::alpha<Checker::Check>(pi->dom(), join->op(i)))
            arm->blame("domain type `{}` of test-expression arm does not match union case type `{}`", pi->dom(),
                       join->op(i))
                .bail();
        type = type ? this->join({type, pi->codom()}) : pi->codom();
    }

    // A constructor fixes the active union case. Dispatch before the Match can
    // escape into later lowering phases, where the payload representation may
    // already have changed (for example, a tensor may have become a buffer).
    if (auto inj = scrutinee->isa<Inj>()) {
        for (size_t i = 0, e = arms.size(); i != e; ++i)
            if (Checker::alpha<Checker::Check>(inj->value()->unfold_type(), join->op(i)))
                return app(arms[i], inj->value());
        scrutinee->blame("injected value type `{}` is not a case of union type `{}`", type_of(inj->value()), join)
            .bail();
    }

    return unify<Match>(type, ops);
}

const Def* World::uniq(const Def* inhabitant) {
    inhabitant = inhabitant->zonk();
    // A singleton type sits one level above its inhabitant, so the top of the hierarchy has none.
    auto t = inhabitant->unfold_type();
    if (auto tt = t ? t->unfold_type() : nullptr) return unify<Uniq>(tt, inhabitant);
    inhabitant->blame("`{}` is too high in the universe hierarchy to inhabit a singleton type", inhabitant).bail();
}

Sym World::append_suffix(Sym symbol, std::string suffix) {
    auto name = symbol.str();

    auto pos = name.find(suffix);
    if (pos != std::string::npos) {
        auto num = name.substr(pos + suffix.size());
        if (num.empty()) {
            name += "_1";
        } else {
            num  = num.substr(1);
            num  = std::to_string(std::stoi(num) + 1);
            name = name.substr(0, pos + suffix.size()) + "_" + num;
        }
    } else {
        name += suffix;
    }

    return sym(std::move(name));
}

Defs World::reduce(const Var* var, const Def* arg) {
    auto mut = var->binder();
    auto off = mut->reduction_offset();
    auto n   = mut->num_ops() - off;
    if (var == arg) return {mut->ops().begin() + off, n}; // `[var -> var]` is the identity

    auto reduct = this->reduct(var, arg, n);
    auto rw     = VarRewriter(var, arg); // one rewriter for all slots: they share their sub-rewrites
    for (size_t i = 0; i != n; ++i) {
        auto& slot = reduct->ops()[i];
        if (slot) continue;
        assert(slot != Filling && "op requires its own reduction");
        slot = Filling;
        slot = rw.rewrite(mut->op(i + off));
    }

    return reduct->ops();
}

const Def* World::cached_reduct(const Var* var, const Def* arg, size_t i) {
    if (auto it = move_.substs.find(std::pair{var, arg}); it != move_.substs.end())
        if (auto slot = it->second->ops()[i]; slot != Filling) return slot;
    return nullptr;
}

const Def* World::reduce(const Var* var, const Def* arg, size_t i) {
    auto mut = var->binder();
    auto off = mut->reduction_offset();
    if (var == arg) return mut->op(i + off); // `[var -> var]` is the identity

    auto reduct = this->reduct(var, arg, mut->num_ops() - off);
    auto& slot  = reduct->ops()[i];
    assert(slot != Filling && "op requires its own reduction");
    if (!slot) {
        auto op = mut->op(i + off);
        if (!op) fe::throwf("cannot reduce `{}`: operand {} is not set", mut, i + off);
        if (!op->has_free_vars_in(Vars(var))) return slot = op; // no occurrence: don't even build a VarRewriter
        slot = Filling;
        slot = VarRewriter(var, arg).rewrite(op);
    }

    return slot;
}

void World::for_each(bool elide_empty, std::function<void(Def*)> f, bool schedule /* = false */) {
    fe::BFSWorklist<MutSet> queue;
    for (auto mut : externals().muts())
        queue.push(mut);

    auto muts = fe::Vector<Def*>();
    while (!queue.empty()) {
        auto mut = queue.pop();
        if (mut->is_closed() && (!elide_empty || mut->is_set())) muts.emplace_back(mut);

        for (auto op : mut->deps())
            for (auto local_mut : op->local_muts())
                queue.push(local_mut);
    }

    // Schedules the mutables in post-order to ensure that they
    // are emitted in the correct order of dependencies.
    if (schedule && !muts.empty()) { // Nest takes its World from the first mutable, so it needs one
        const auto mut_nest = Nest(muts);
        auto schedule       = Scheduler::schedule(mut_nest) | std::views::reverse | std::views::filter([&](Def* mut) {
                            return mut->is_closed() && (!elide_empty || mut->is_set());
                        });
        for (auto* mut : schedule)
            f(mut);
    } else {
        for (auto* mut : muts)
            f(mut);
    }
}

/*
 * debugging
 */

#ifdef MIM_ENABLE_CHECKS

void World::breakpoint(u32 gid) { state_.breakpoints.emplace(gid); }
void World::watchpoint(u32 gid) { state_.watchpoints.emplace(gid); }

const Def* World::gid2def(u32 gid) {
    auto i = std::ranges::find_if(move_.sea, [=](auto def) { return def->gid() == gid; });
    if (i == move_.sea.end()) return nullptr;
    return *i;
}

World& World::verify() {
    for (auto mut : externals().muts())
        assert(mut->is_closed() && mut->is_set());
    for (auto anx : annexes().defs())
        assert(anx->is_closed());
    return *this;
}

#endif

#ifndef DOXYGEN
template const Def* World::umax<UMax::Term>(Defs);
template const Def* World::umax<UMax::Type>(Defs);
template const Def* World::umax<UMax::Kind>(Defs);
template const Def* World::umax<UMax::Univ>(Defs);
template const Def* World::ext<true>(const Def*);
template const Def* World::ext<false>(const Def*);
template const Def* World::bound<true>(Defs);
template const Def* World::bound<false>(Defs);
template const Def* World::app<true>(const Def*, const Def*);
template const Def* World::app<false>(const Def*, const Def*);
template const Def* World::implicit_app<true>(const Def*, const Def*);
template const Def* World::implicit_app<false>(const Def*, const Def*);
#endif

// Interning here - once per push - instead of in unify() keeps ~170k redundant Driver::dbg lookups per compile
// off the hot path: only a few thousand distinct Loc%s occur, yet every emitted Def wants one.
// Restore rolls both fields back together, so popping a scope never re-interns either.
World::ScopedLoc World::push(Loc loc) {
    auto& curr = state_.pod.curr_loc;
    if (loc == curr.loc) return ScopedLoc(curr); // nested emitters push the same Loc; don't re-intern it
    return ScopedLoc(curr, {loc, loc ? driver().dbg(Dbg(loc)) : DbgKey()});
}

} // namespace mim
