#include "mim/world.h"

#include "mim/check.h"
#include "mim/def.h"
#include "mim/driver.h"
#include "mim/rewrite.h"
#include "mim/rule.h"
#include "mim/tuple.h"

#include "mim/util/util.h"

namespace mim {

namespace {

bool is_shape(const Def* s) {
    if (s->isa<Nat>()) return true;
    if (auto arr = s->isa<Arr>()) return arr->body()->zonk()->isa<Nat>();
    if (auto sig = s->isa_imm<Sigma>())
        return std::ranges::all_of(sig->ops(), [](const Def* op) { return op->isa<Nat>(); });

    return false;
}

} // namespace

void World::Externals::externalize(Def* def) {
    assert(!def->is_external());
    assert(def->is_closed());
    def->external_ = true;
    assert_emplace(sym2mut_, def->sym(), def);
}

void World::Externals::internalize(Def* def) {
    assert(def->is_external());
    def->external_ = false;
    auto num       = sym2mut_.erase(def->sym());
    assert_unused(num == 1);
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
    , state_(state) {
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

World::World(Driver* driver)
    : World(driver, State()) {}

World::~World() {
    for (auto def : move_.defs)
        def->~Def();
}

/*
 * Driver
 */

Log& World::log() const { return driver().log(); }
Flags& World::flags() { return driver().flags(); }

Sym World::sym(const char* s) { return driver().sym(s); }
Sym World::sym(std::string_view s) { return driver().sym(s); }
Sym World::sym(const std::string& s) { return driver().sym(s); }

const Def* World::register_annex(flags_t f, const Def* def) {
    TLOG("register: 0x{x} -> {}", f, def);
    auto plugin = Annex::demangle(driver(), f);
    if (driver().is_loaded(plugin)) {
        assert_emplace(move_.flags2annex, f, def);
        def->annex_ = true;
        return def;
    }
    return nullptr;
}

/*
 * factory methods
 */

const Type* World::type(const Def* level) {
    if (!level) return nullptr;
    level = level->zonk();

    if (!level->type()->isa<Univ>())
        error(level->loc(), "argument `{}` to `Type` must be of type `Univ` but is of type `{}`", level, level->type());

    return unify<Type>(level)->as<Type>();
}

const Def* World::uinc(const Def* op, level_t offset) {
    op = op->zonk();

    if (!op->type()->isa<Univ>())
        error(op->loc(), "operand '{}' of a universe increment must be of type `Univ` but is of type `{}`", op,
              op->type());

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
    for (auto op : ops_) {
        op = op->zonk();

        if constexpr (sort == UMax::Term) op = op->unfold_type();
        if constexpr (sort >= UMax::Type) op = op->unfold_type();
        if constexpr (sort >= UMax::Kind) {
            if (auto type = op->isa<Type>())
                op = type->level();
            else
                error(op->loc(), "operand '{}' must be a Type of some level", op); // TODO better error message
        }

        flatten_umax(ops, op);
    }

    level_t lvl = 0;
    DefVec res;
    for (auto op : ops) {
        if (!op->type()->isa<Univ>())
            error(op->loc(), "operand '{}' of a universe max must be of type 'Univ' but is of type '{}'", op,
                  op->type());

        if (auto l = Lit::isa(op))
            lvl = std::max(lvl, *l);
        else
            res.emplace_back(op);
    }

    const Def* l = lit_univ(lvl);
    if (res.empty()) return sort == UMax::Univ ? l : type(l);
    if (lvl > 0) res.emplace_back(l);

    std::ranges::sort(res, [](auto op1, auto op2) { return op1->gid() < op2->gid(); });
    res.erase(std::unique(res.begin(), res.end()), res.end());
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
    while (auto pi = Pi::isa_implicit(callee->type()))
        callee = app(callee, mut_hole(pi->dom()));
    return app<Normalize>(callee, arg);
}

template<bool Normalize>
const Def* World::app(const Def* callee, const Def* arg) {
    callee = callee->zonk();
    arg    = arg->zonk();

    if (auto pi = callee->type()->isa<Pi>()) {
        if (auto new_arg = Checker::assignable(pi->dom(), arg)) {
            arg = new_arg->zonk();
            if (auto imm = callee->isa_imm<Lam>()) return imm->body();

            if (auto lam = callee->isa_mut<Lam>(); lam && lam->is_set() && lam->filter() != lit_ff()) {
                if (auto var = lam->has_var()) {
                    if (auto i = move_.substs.find({var, arg}); i != move_.substs.end()) {
                        // Is there a cached version?
                        auto [filter, body] = i->second->defs<2>();
                        if (filter == lit_tt()) return body;
                    } else {
                        // First check filter, If true, reduce body and cache reduct.
                        auto rw     = VarRewriter(var, arg);
                        auto filter = rw.rewrite(lam->filter());
                        if (filter == lit_tt()) {
                            DLOG("partial evaluate: {} ({})", lam, arg);
                            auto body        = rw.rewrite(lam->body());
                            auto num_bytes   = sizeof(Reduct) + 2 * sizeof(const Def*);
                            auto buf         = move_.arena.substs.allocate(num_bytes, alignof(const Def*));
                            auto reduct      = new (buf) Reduct(2);
                            reduct->defs_[0] = filter;
                            reduct->defs_[1] = body;
                            assert_emplace(move_.substs, std::pair{var, arg}, reduct);
                            return body;
                        }
                    }
                } else if (lam->filter() == lit_tt()) {
                    return lam->body();
                }
            }

            auto type               = pi->reduce(arg)->zonk();
            callee                  = callee->zonk();
            auto [axm, curry, trip] = Axm::get(callee);
            if (axm) {
                curry = curry == 0 ? trip : curry;
                curry = curry == Axm::Trip_End ? curry : curry - 1;

                if (auto normalizer = axm->normalizer(); Normalize && normalizer && curry == 0) {
                    if (auto norm = normalizer(type, callee, arg)) return norm;
                }
            }

            return raw_app(axm, curry, trip, type, callee, arg);
        }

        throw Error()
            .error(arg->loc(), "cannot apply argument to callee")
            .note(callee->loc(), "callee: '{}'", callee)
            .note(arg->loc(), "argument: '{}'", arg)
            .note(callee->loc(), "vvv domain type vvv\n'{}'\n'{}'", pi->dom(), arg->type())
            .note(arg->loc(), "^^^ argument type ^^^");
    }

    throw Error()
        .error(callee->loc(), "called expression not of function type")
        .error(callee->loc(), "'{}' <--- callee type", callee->type());
}

const Def* World::raw_app(const Def* type, const Def* callee, const Def* arg) {
    type   = type->zonk();
    callee = callee->zonk();
    arg    = arg->zonk();

    auto [axm, curry, trip] = Axm::get(callee);
    if (axm) {
        curry = curry == 0 ? trip : curry;
        curry = curry == Axm::Trip_End ? curry : curry - 1;
    }

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
        error(t->loc(), "cannot assign tuple '{}' of type '{}' to incompatible tuple type '{}'", t, t->type(), sigma);

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

    if (n != 0) {
        // eta rule for tuples:
        // (extract(tup, 0), extract(tup, 1), extract(tup, 2)) -> tup
        if (auto extract = ops[0]->isa<Extract>()) {
            auto tup = extract->tuple();
            bool eta = tup->type() == type;
            for (size_t i = 0; i != n && eta; ++i) {
                if (auto extract = ops[i]->isa<Extract>()) {
                    if (auto index = Lit::isa(extract->index())) {
                        if (eta &= u64(i) == *index) {
                            eta &= extract->tuple() == tup;
                            continue;
                        }
                    }
                }
                eta = false;
            }

            if (eta) return tup;
        }
    }

    return unify<Tuple>(type, ops);
}

const Def* World::tuple(Sym sym) {
    DefVec defs;
    std::ranges::transform(sym, std::back_inserter(defs), [this](auto c) { return lit_i8(c); });
    return tuple(defs);
}

bool isa_indicies(const Def* def) {
    if (Idx::isa(def)) return true;
    if (auto sigma = def->isa<Sigma>()) return std::ranges::all_of(sigma->ops(), [](auto op) { return Idx::isa(op); });
    if (auto arr = def->isa<Arr>()) return Idx::isa(arr->body());
    return false;
}

const Def* World::extract(const Def* d, const Def* index) {
    if (!d || !index) return nullptr; // can happen if frozen
    d     = d->zonk();
    index = index->zonk();

    if (!isa_indicies(index->type()))
        error(index->loc(), "index '{}' is not of Idx type but of type '{}'", index, index->type());

    if (auto tuple = index->isa<Tuple>()) {
        for (auto op : tuple->ops())
            d = extract(d, op);
        return d;
    } else if (auto pack = index->isa<Pack>()) {
        if (auto a = Lit::isa(index->arity())) {
            for (nat_t i = 0, e = *a; i != e; ++i) {
                auto idx = pack->has_var() ? pack->reduce(lit_idx(*a, i)) : pack->body();
                d        = extract(d, idx);
            }
            return d;
        }
    }

    auto size = Idx::isa(index->type());
    auto type = d->unfold_type();

    if (size) {
        if (auto l = Lit::isa(size); l && *l == 1) {
            if (auto l = Lit::isa(index); !l || *l != 0) WLOG("unknown Idx of size 1: {}", index);
            if (auto sigma = type->isa_mut<Sigma>(); sigma && sigma->num_ops() == 1) {
                // mut sigmas can be 1-tuples; TODO mutables Arr?
            } else {
                return d;
            }
        }
    }

    if (auto pack = d->isa_imm<Pack>()) return pack->body();

    if (size && !Checker::alpha<Checker::Check>(type->arity(), size))
        error(index->loc(), "index '{}' does not fit within arity '{}'", index, type->arity());
    // TODO if we have indices we need to check as well that this is compatible with `d`

    // extract(insert(x, index, val), index) -> val
    if (auto insert = d->isa<Insert>()) {
        if (index == insert->index()) return insert->value();
    }

    if (auto i = Lit::isa(index)) {
        if (auto hole = d->isa_mut<Hole>()) d = hole->tuplefy(Idx::as_lit(index->type()));
        if (auto tuple = d->isa<Tuple>()) return tuple->op(*i);

        // extract(insert(x, j, val), i) -> extract(x, i) where i != j (guaranteed by rule above)
        if (auto insert = d->isa<Insert>()) {
            if (insert->index()->isa<Lit>()) return extract(insert->tuple(), index);
        }

        if (auto sigma = type->isa<Sigma>()) {
            if (auto var = sigma->has_var()) {
                if (is_frozen()) return nullptr; // if frozen, we don't risk rewriting
                auto t = VarRewriter(var, d).rewrite(sigma->op(*i));
                return unify<Extract>(t, d, index);
            }

            return unify<Extract>(sigma->op(*i), d, index);
        }
    }

    const Def* elem_t;
    if (auto arr = type->isa<Arr>())
        elem_t = arr->reduce(index);
    else
        elem_t = join(type->as<Sigma>()->ops());

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
    auto size = Idx::isa(index->type());
    auto lidx = Lit::isa(index);

    if (!size) error(d->loc(), "index '{}' must be of type 'Idx' but is of type '{}'", index, index->type());

    if (!Checker::alpha<Checker::Check>(type->arity(), size))
        error(index->loc(), "index '{}' does not fit within arity '{}'", index, type->arity());

    if (lidx) {
        auto elem_type = type->proj(*lidx);
        auto new_val   = Checker::assignable(elem_type, val);
        if (!new_val) {
            throw Error()
                .error(val->loc(), "value to be inserted not assignable to element")
                .note(val->loc(), "vvv value type vvv \n'{}'\n'{}'", val->type(), elem_type)
                .note(val->loc(), "^^^ element type ^^^", elem_type);
        }
        val = new_val;
    }

    if (auto l = Lit::isa(size); l && *l == 1)
        return tuple(d, {val}); // d could be mut - that's why the tuple ctor is needed

    // insert((a, b, c, d), 2, x) -> (a, b, x, d)
    if (auto t = d->isa<Tuple>(); t && lidx) return t->refine(*lidx, val);

    // insert(‹4; x›, 2, y) -> (x, x, y, x)
    if (auto pack = d->isa<Pack>(); pack && lidx) {
        if (auto a = Lit::isa(pack->arity()); a && *a < flags().scalarize_threshold) {
            auto new_ops   = DefVec(*a, pack->body());
            new_ops[*lidx] = val;
            return tuple(type, new_ops);
        }
    }

    // insert(insert(x, index, y), index, val) -> insert(x, index, val)
    if (auto insert = d->isa<Insert>()) {
        if (insert->index() == index) d = insert->tuple();
    }

    return unify<Insert>(d, index, val);
}

const Def* World::seq(bool term, const Def* arity, const Def* body) {
    arity = arity->zonk(); // TODO use zonk_mut all over the place and rmeove zonk from is_shape?
    body  = body->zonk();

    auto arity_ty = arity->unfold_type();
    if (!is_shape(arity_ty)) error(arity->loc(), "expected arity but got `{}` of type `{}`", arity, arity_ty);

    if (auto a = Lit::isa(arity)) {
        if (*a == 0) return unit(term);
        if (*a == 1) return body;
    }

    // «(a, b, c); body» -> «a; «(b, c); body»»
    // e.g. when var, but still has array type
    if (auto arr_arity = arity->type()->isa<Seq>())
        if (auto lit_arity_arity = Lit::isa(arr_arity->arity())) {
            DefVec inner_arity(*lit_arity_arity - 1, [&](u64 i) { return arity->proj(*lit_arity_arity, i + 1); });
            return seq(term, arity->proj(*lit_arity_arity, 0), seq(term, tuple(inner_arity), body));
        }


    if (term) {
        auto type = arr(arity, body->type());
        return unify<Pack>(type, body);
    } else {
        return unify<Arr>(body->unfold_type(), arity, body);
    }
}

const Def* World::seq(bool term, Defs shape, const Def* body) {
    if (shape.empty()) return body;
    return seq(term, shape.rsubspan(1), seq(term, shape.back(), body));
}

const Lit* World::lit(const Def* type, u64 val) {
    if (!type) return nullptr;
    type = type->zonk();

    if (auto size = Idx::isa(type)) {
        if (size->isa<Top>()) {
            // unsafe but fine
        } else if (auto s = Lit::isa(size)) {
            if (*s != 0 && val >= *s) error(type->loc(), "index '{}' does not fit within arity '{}'", size, val);
        } else if (val != 0) { // 0 of any size is allowed
            error(type->loc(), "cannot create literal '{}' of 'Idx {}' as size is unknown", val, size);
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

    if (auto arr = type->isa<Arr>()) return pack(arr->arity(), ext<Up>(arr->body()));
    if (auto sigma = type->isa<Sigma>())
        return tuple(sigma, DefVec(sigma->num_ops(), [&](size_t i) { return ext<Up>(sigma->op(i)); }));
    return unify<TExt<Up>>(type);
}

template<bool Up>
const Def* World::bound(Defs ops_) {
    auto ops = DefVec();
    for (size_t i = 0, e = ops_.size(); i != e; ++i) {
        auto op = ops_[i]->zonk();
        if (!op->isa<TExt<!Up>>()) ops.emplace_back(op); // ignore: ext<!Up>
    }

    auto kind = umax<UMax::Type>(ops);

    // has ext<Up> value?
    if (std::ranges::any_of(ops, [&](const Def* op) -> bool { return op->isa<TExt<Up>>(); })) return ext<Up>(kind);

    // sort and remove duplicates
    std::ranges::sort(ops, GIDLt<const Def*>());
    ops.resize(std::distance(ops.begin(), std::unique(ops.begin(), ops.end())));

    if (ops.size() == 0) return ext<!Up>(kind);
    if (ops.size() == 1) return ops[0];

    // TODO simplify mixed terms with joins and meets?
    return unify<TBound<Up>>(kind, ops);
}

const Def* World::merge(const Def* type, Defs ops_) {
    type     = type->zonk();
    auto ops = Def::zonk(ops_);

    if (type->isa<Meet>()) {
        auto types = DefVec(ops.size(), [&](size_t i) { return ops[i]->type(); });
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
    auto join      = scrutinee->type()->isa<Join>();

    if (!join) error(scrutinee->loc(), "scrutinee of a test expression must be of union type");

    if (arms.size() != join->num_ops())
        error(scrutinee->loc(), "test expression has {} arms but union type has {} cases", arms.size(),
              join->num_ops());

    for (auto arm : arms)
        if (!arm->type()->isa<Pi>())
            error(arm->loc(), "arm of test expression does not have a function type but is of type '{}'", arm->type());

    std::ranges::sort(arms, [](const Def* arm1, const Def* arm2) {
        return arm1->type()->as<Pi>()->dom()->gid() < arm2->type()->as<Pi>()->dom()->gid();
    });

    const Def* type = nullptr;
    for (size_t i = 0, e = arms.size(); i != e; ++i) {
        auto arm = arms[i];
        auto pi  = arm->type()->as<Pi>();
        if (!Checker::alpha<Checker::Check>(pi->dom(), join->op(i)))
            error(arm->loc(),
                  "domain type '{}' of arm in a test expression does not match case type '{}' in union type", pi->dom(),
                  join->op(i));
        type = type ? this->join({type, pi->codom()}) : pi->codom();
    }

    return unify<Match>(type, ops);
}

const Def* World::uniq(const Def* inhabitant) {
    inhabitant = inhabitant->zonk();
    return unify<Uniq>(inhabitant->type()->unfold_type(), inhabitant);
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
    auto mut    = var->mut();
    auto offset = mut->reduction_offset();
    auto size   = mut->num_ops() - offset;

    if (auto i = move_.substs.find({var, arg}); i != move_.substs.end()) return i->second->defs();

    auto buf    = move_.arena.substs.allocate(sizeof(Reduct) + size * sizeof(const Def*), alignof(const Def*));
    auto reduct = new (buf) Reduct(size);
    auto rw     = VarRewriter(var, arg);
    for (size_t i = 0; i != size; ++i)
        reduct->defs_[i] = rw.rewrite(mut->op(i + offset));
    assert_emplace(move_.substs, std::pair{var, arg}, reduct);
    return reduct->defs();
}

void World::for_each(bool elide_empty, std::function<void(Def*)> f) {
    unique_queue<MutSet> queue;
    for (auto mut : externals().muts())
        queue.push(mut);

    while (!queue.empty()) {
        auto mut = queue.pop();
        if (mut && mut->is_closed() && (!elide_empty || mut->is_set())) f(mut);

        for (auto op : mut->deps())
            for (auto mut : op->local_muts())
                queue.push(mut);
    }
}

/*
 * debugging
 */

#ifdef MIM_ENABLE_CHECKS

void World::breakpoint(u32 gid) { state_.breakpoints.emplace(gid); }
void World::watchpoint(u32 gid) { state_.watchpoints.emplace(gid); }

const Def* World::gid2def(u32 gid) {
    auto i = std::ranges::find_if(move_.defs, [=](auto def) { return def->gid() == gid; });
    if (i == move_.defs.end()) return nullptr;
    return *i;
}

World& World::verify() {
    for (auto mut : externals().muts())
        assert(mut->is_closed() && mut->is_set());
    for (auto anx : annexes())
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

} // namespace mim
