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

bool is_shape(const Def* s) {
    if (s->isa<Nat>()) return true;
    if (auto arr = s->isa<Arr>()) return arr->body()->zonk()->isa<Nat>();
    if (auto sig = s->isa_imm<Sigma>())
        return std::ranges::all_of(sig->ops(), [](const Def* op) { return op->isa<Nat>(); });

    return false;
}

/// Is @p def an `Idx` - or an aggregate of `Idx`%s, i.e. a multi-dimensional index?
bool isa_indices(const Def* def) {
    if (!def) return false; // Univ has no type
    if (Idx::isa(def)) return true;
    if (auto sigma = def->isa<Sigma>()) return std::ranges::all_of(sigma->ops(), [](auto op) { return Idx::isa(op); });
    if (auto arr = def->isa<Arr>()) return Idx::isa(arr->body());
    return false;
}

/// Does `d#index \u2190 val` rebuild an aggregate right away? Mirrors the two folding rules in World::insert.
bool insert_rebuilds(const Def* d, const Def* index, u64 threshold) {
    if (!Lit::isa(index)) return false;
    if (d->isa<Tuple>()) return true;
    if (auto pack = d->isa<Pack>(); pack && !pack->is_fused())
        if (auto a = Lit::isa(pack->arity())) return *a < threshold;
    return false;
}

/// The Node a Seq's body must have to be fused into it.
constexpr Node body_node(bool is_pack) { return is_pack ? Node::Pack : Node::Arr; }

/// `(a, b)` from shapes @p a and @p b; `nullptr` unless both ranks are known.
const Def* cat_shape(const Def* a, const Def* b) {
    auto ra = Lit::isa(a->arity());
    auto rb = Lit::isa(b->arity());
    if (!ra || !rb) return nullptr;
    return cat_tuple(*ra, *rb, a, b);
}

/// Checks the multi-dimensional @p index against @p d and yields the type it peels to.
const Def* nary_elem_type(const Def* d, const Def* index, const Def* index_ty) {
    auto type = d->unfold_type();
    auto arr  = type->isa<Arr>();
    if (!arr) index->blame("multi-dimensional index `{}` expects an array but got `{}`", index, type).bail();

    auto ri  = Lit::isa(index_ty->arity());
    auto rt  = Lit::isa(arr->rank());
    auto fit = ri && rt ? *ri <= *rt // a *partial* index peels only its own axes and yields a sub-array
                        : Checker::alpha<Checker::Check>(index_ty->arity(), arr->rank());
    if (!fit)
        index
            ->blame("index `{}` of rank `{}` does not fit shape `{}` of rank `{}`", index, index_ty->arity(),
                    arr->shape(), arr->rank())
            .bail();

    return d->world().peel(arr, index);
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

const Def* World::extract(const Def* d, const Def* index) {
    if (!d || !index) return nullptr; // can happen if frozen
    d     = d->zonk();
    index = index->zonk();

    auto index_ty = index->unfold_type();
    // The scalar case is by far the most common one, so probe it first and only fall back to the aggregate check.
    if (!Idx::isa(index_ty) && !isa_indices(index_ty))
        index->blame("index `{}` must be of `Idx` type but is of type `{}`", index, type_of(index)).bail();

    // A *scalar* index keeps its `Idx 1` carve-out for mutable 1-tuples; only a multi-dimensional one drops
    // those components, mirroring the size-1 axes World::seq folds out of `d`'s shape: `d#(i, 0₁, k)` ≡ `d#(i, k)`.
    if (Idx::isa(index_ty)) return extract1(d, index);
    index    = fold_index(index);
    index_ty = index->unfold_type();
    if (Idx::isa(index_ty)) return extract1(d, index);

    auto r = Lit::isa(index_ty->arity());
    if (r && *r == 0) return d; // the index folded away entirely, i.e. `d` is its own only element

    // Fold from the left as far as `d` allows; whatever is left stays *fused*, mirroring `«(a, b); T»`.
    for (nat_t i = 0; r && i != *r; ++i) {
        auto c    = index->proj(*r, i);
        auto next = extract1(d, c);
        if (auto ex = next->isa<Extract>(); ex && ex->tuple() == d && ex->index() == c) {
            index = i == 0 ? index : tuple(DefVec(*r - i, [&](size_t j) { return index->proj(*r, i + j); }));
            return extract_fused(d, index);
        }
        d = next;
    }
    if (r) return d;

    return extract_fused(d, index);
}

const Def* World::drop(const Seq* s, nat_t k) {
    auto r = Lit::isa(s->rank());
    if (!r || k > *r || s->has_var()) return nullptr; // a dependent shape needs an index; cf. World::peel
    if (k == *r) return s->body();
    if (is_frozen()) return nullptr; // splitting a fused shape builds the sub-Seq
    auto tail = tuple(DefVec(*r - k, [&](size_t i) { return s->shape()->proj(*r, k + i); }));
    return seq(s->node() == Node::Pack, tail, s->body());
}

const Def* World::peel(const Seq* seq, const Def* index) {
    if (!index) return nullptr; // can happen if frozen
    auto rank = Lit::isa(seq->rank());
    auto k    = Lit::isa(index->unfold_type()->arity());
    if (!rank || !k || *k >= *rank) return seq->reduce(index); // the index covers every axis

    if (is_frozen()) return nullptr; // splitting a fused shape builds the sub-Seq
    auto is_pack = seq->node() == Node::Pack;
    auto tail    = tuple(DefVec(*rank - *k, [&](size_t i) { return seq->shape()->proj(*rank, *k + i); }));
    if (!seq->has_var()) return this->seq(is_pack, tail, seq->body());

    // Dependent: the leading axes come from @p index, the trailing ones from the result's own index.
    auto rest = [&](Seq* res) { return seq->reduce(cat_tuple(*k, *rank - *k, index, res->var())); };
    auto res  = mut_seq(is_pack, is_pack ? arr(tail, seq->body()->unfold_type()) : seq->type());
    res->Def::set(0, tail);
    return res->Def::set(1, rest(res))->zonk_mut();
}

const Def* World::type_indices(const Def* shape) {
    if (is_dim(shape)) return type_idx(shape);
    if (auto r = Lit::isa(shape->arity()))
        return sigma(DefVec(*r, [&](size_t i) { return type_idx(shape->proj(*r, i)); }));

    // `«i: r; Idx (s#i)»` - a dynamic rank cannot spell out its axes.
    auto res = mut_arr(type())->set_shape(shape->arity());
    return res->set_body(type_idx(extract(shape, res->var())))->zonk_mut();
}

/// Builds `d#index` for a multi-dimensional @p index that does not fold any further.
const Def* World::extract_fused(const Def* d, const Def* index) {
    auto index_ty = index->unfold_type();
    if (Idx::isa(index_ty)) return extract1(d, index);

    // An index reaching past `d`'s own shape continues through the element type - `«2; [Nat, Bool]»` is rank 1,
    // yet `t#(i, j)` is still `t#i#j`. Split it at the boundary, so each Extract matches the shape it indexes.
    auto ri  = Lit::isa(index_ty->arity());
    auto arr = d->unfold_type()->isa<Arr>();
    auto rt  = arr ? Lit::isa(arr->rank()) : std::optional<nat_t>(ri ? 1 : 0);
    if (ri && rt && *rt != 0 && *ri > *rt && !is_frozen()) {
        auto head = tuple(DefVec(*rt, [&](size_t i) { return index->proj(*ri, i); }));
        auto tail = tuple(DefVec(*ri - *rt, [&](size_t i) { return index->proj(*ri, *rt + i); }));
        return extract(extract_fused(d, head), tail);
    }

    // `d#is ← val` then `#is` -> `val`; cf. the same rule in World::extract1
    if (auto insert = d->isa<Insert>(); insert && insert->index() == index) return insert->value();
    return unify<Extract>(nary_elem_type(d, index, index_ty), d, index);
}

const Def* World::extract1(const Def* d, const Def* index) {
    auto index_ty = index->unfold_type();
    auto size     = Idx::isa(index_ty);
    auto lidx     = Lit::isa(index);
    auto type     = d->unfold_type();

    if (auto l = Lit::isa(size); l && *l == 1) {
        if (!lidx || *lidx != 0) log().w("index of `Idx 1` is not the literal 0: {}", index);
        // A *mutable* Sigma may be a 1-tuple and still needs a real Extract; TODO mutable Arr?
        auto sigma = type->isa_mut<Sigma>();
        if (!sigma || sigma->num_ops() != 1) return d;
    }

    if (!Checker::alpha<Checker::Check>(type->arity(), size))
        index->blame("index `{}` does not fit within arity `{}`", index, type->arity()).bail();

    if (auto pack = d->isa<Pack>()) {
        if (pack->is_fused()) return peel(pack, index); // one axis off a fused Pack yields a sub-Pack
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
    if (auto ex = is_frozen() ? nullptr : d->isa<Extract>()) {
        auto t = ex->tuple();
        if (auto arr = t->unfold_type()->isa<Arr>()) {
            auto ri = Lit::isa(ex->index()->unfold_type()->arity());
            auto rt = Lit::isa(arr->rank());
            if (ri && rt && *ri + 1 <= *rt) return extract_fused(t, cat_tuple(*ri, 1, ex->index(), index));
        }
    }

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
                if (is_frozen()) return nullptr; // if frozen, we don't risk rewriting
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
                extents.emplace_back(op_arr->shape());
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

const Def* World::insert(const Def* d, const Def* index, const Def* val) {
    d     = d->zonk();
    index = index->zonk();
    val   = val->zonk();

    auto type = d->unfold_type();
    auto size = Idx::isa(index->unfold_type());
    auto lidx = Lit::isa(index);

    // `d#(i, 0₁, k) ← val` ≡ `d#(i, k) ← val`: mirrors the size-1 axis World::seq folds out of `d`'s shape.
    // A *scalar* index is left alone; cf. World::extract.
    if (!size) {
        if (!isa_indices(index->unfold_type()))
            index->blame("index `{}` must be of `Idx` type but is of type `{}`", index, type_of(index)).bail();
        index = fold_index(index);
        size  = Idx::isa(index->unfold_type());
        lidx  = Lit::isa(index);
    }

    // `d#(i, j, k) ← val` stays *fused* unless the outermost write rebuilds an aggregate on the spot.
    if (!size) {
        auto index_ty = index->unfold_type();
        auto r        = Lit::isa(index_ty->arity());
        if (r && *r == 0) return val; // the index folded away entirely, so the write replaces all of `d`

        // A write deeper than `d`'s own shape reads down to the boundary and writes the levels back out;
        // cf. World::extract_fused. So does one the outermost write can rebuild an aggregate for.
        auto arr = type->isa<Arr>();
        auto rt  = arr ? Lit::isa(arr->rank()) : std::optional<nat_t>(r ? 1 : 0);
        auto cut = r && rt && *rt != 0 && *r > *rt                                          ? std::optional<nat_t>(*rt)
                 : r && insert_rebuilds(d, index->proj(*r, 0), flags().scalarize_threshold) ? std::optional<nat_t>(1)
                                                                                            : std::nullopt;
        if (cut) {
            auto head = tuple(DefVec(*cut, [&](size_t i) { return index->proj(*r, i); }));
            auto rest = tuple(DefVec(*r - *cut, [&](size_t i) { return index->proj(*r, *cut + i); }));
            return insert(d, head, insert(extract(d, head), rest, val));
        }

        auto elem_type = nary_elem_type(d, index, index_ty);
        auto new_val   = Checker::assignable(elem_type, val);
        if (!new_val)
            val->blame("value is not assignable to element type")
                .n("expected `{}`, got `{}`", elem_type, type_of(val))
                .n("value: `{}`", val)
                .bail();

        // `d#is ← d#is` -> `d` and `(d#is ← y)#is ← val` -> `d#is ← val`; cf. the scalar path below
        if (auto extract = new_val->isa<Extract>(); extract && extract->tuple() == d && extract->index() == index)
            return d;
        if (auto insert = d->isa<Insert>(); insert && insert->index() == index) d = insert->tuple();
        return unify<Insert>(d, index, new_val);
    }

    if (auto l = Lit::isa(size); l && *l == 1) {
        if (!lidx || *lidx != 0) log().w("index of `Idx 1` is not the literal 0: {}", index);
        // A size-1 axis folds out of the type, so `d#index` is `d` itself (cf. World::extract) and the insert
        // replaces all of it. A *mutable* Sigma may be a genuine 1-tuple and is handled below.
        auto sigma = type->isa_mut<Sigma>();
        if (!sigma || sigma->num_ops() != 1) return val;
    }

    if (!Checker::alpha<Checker::Check>(type->arity(), size))
        index->blame("index `{}` does not fit within arity `{}`", index, type->arity()).bail();

    if (lidx) {
        auto elem_type = type->proj(*lidx);
        auto new_val   = Checker::assignable(elem_type, val);
        if (!new_val) {
            val->blame("value is not assignable to element type")
                .n("expected `{}`, got `{}`", elem_type, type_of(val))
                .n("value: `{}`", val)
                .bail();
        }
        val = new_val;
    }

    if (auto l = Lit::isa(size); l && *l == 1)
        return tuple(d, {val}); // a mutable 1-tuple: d could be mut - that's why the tuple ctor is needed

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
        if (auto ex = inner->tuple()->isa<Extract>(); ex && ex->tuple() == d && ex->index() == index)
            if (auto arr = type->isa<Arr>()) {
                auto rj = Lit::isa(inner->index()->unfold_type()->arity());
                auto rt = Lit::isa(arr->rank());
                if (rj && rt && 1 + *rj <= *rt)
                    return insert(d, cat_tuple(1, *rj, index, inner->index()), inner->value());
            }

    // insert(d, index, d#index) -> d
    if (auto extract = val->isa<Extract>())
        if (extract->tuple() == d && extract->index() == index) return d;

    // insert(insert(x, index, y), index, val) -> insert(x, index, val)
    if (auto insert = d->isa<Insert>()) {
        if (insert->index() == index) d = insert->tuple();
    }

    return unify<Insert>(d, index, val);
}

const Def* World::seq(bool is_pack, const Def* shape, const Def* body) {
    shape = shape->zonk();
    body  = body->zonk();

    auto shape_ty = shape->unfold_type();
    if (!is_shape(shape_ty)) shape->blame("expected shape but got `{}` of type `{}`", shape, shape_ty).bail();

    // `«1; T»` ≡ `T`, mirroring `[T]` ≡ `T`, so a literal size-1 axis folds out of the shape.
    shape  = fold_shape(shape);
    auto r = Lit::isa(shape->arity());
    if (r) {
        if (*r == 0) return body;
        // A literal-`0` extent empties everything below it: `«(2, 0, 3); T»` is `«2; []»`, not `[]`.
        for (size_t i = 0; i != *r; ++i)
            if (auto e = Lit::isa(shape->proj(*r, i)); e && *e == 0)
                return seq(is_pack, DefVec(i, [&](size_t j) { return shape->proj(*r, j); }), unit(is_pack));
    }

    // `«a; «b; T»»` ≡ `«(a, b); T»` - the very compression that already makes `«3; T»` out of `[T, T, T]`,
    // one level up. A *mutable* body binds an index the fused shape could not express, so it stops fusion.
    if (auto inner = body->isa_imm<Seq>(); inner && inner->node() == body_node(is_pack))
        if (auto fused = cat_shape(shape, inner->shape())) return seq(is_pack, fused, inner->body());

    if (is_pack) return unify<Pack>(arr(shape, body->unfold_type()), shape, body);
    return unify<Arr>(body->unfold_type(), shape, body);
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

const Def* World::reduce(const Var* var, const Def* arg, size_t i) {
    auto mut = var->binder();
    auto off = mut->reduction_offset();
    if (var == arg) return mut->op(i + off); // `[var -> var]` is the identity

    auto reduct = this->reduct(var, arg, mut->num_ops() - off);
    auto& slot  = reduct->ops()[i];
    assert(slot != Filling && "op requires its own reduction");
    if (!slot) {
        auto op = mut->op(i + off);
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
    if (schedule) {
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
