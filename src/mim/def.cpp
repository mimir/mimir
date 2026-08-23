#include "mim/def.h"

#include <algorithm>

#include <fe/assert.h>
#include <fe/hash.h>

#include "mim/driver.h"
#include "mim/rule.h"
#include "mim/world.h"

using namespace std::literals;

namespace mim {

template void Sets<const Var>::dot();
template void Sets<Def>::dot();

/*
 * constructors
 */

// The Deps a node contributes to *itself*; Def::dep_ then accumulates the ones of its deps() on top.
static constexpr unsigned node2dep(Node node, bool mut) {
    auto dep = mut ? Dep::Mut : Dep::None;
    // clang-format off
    switch (node) {
        case Node::Hole:  dep |= Dep::Hole;           break;
        case Node::Proxy: dep |= Dep::Proxy;          break;
        case Node::Var:   dep |= Dep::Var | Dep::Mut; break;
        default:                                      break;
    }
    // clang-format on
    return fe::to_underlying(dep);
}

Def::Def(World* world, Node node, const Def* type, Defs ops, flags_t flags)
    : world_(world)
    , flags_(flags)
    , node_(node)
    , mut_(false)
    , external_(false)
    , annex_(false)
    , dirty_(false)
    , dep_(node2dep(node, false))
    , num_ops_(ops.size())
    , type_(type) {
    if (node == Node::Univ) {
        gid_  = world->next_gid();
        hash_ = fe::hash_begin(node_t(Node::Univ));
    } else {
        hash_ = fe::hash_begin(u8(node));
        hash_ = fe::hash_combine(hash_, flags_);

        if (type) {
            world = &type->world();
            dep_ |= type->dep_;
            vars_ = type->local_vars();
            muts_ = type->local_muts();
            hash_ = fe::hash_combine(hash_, type->gid());
        } else {
            world = &ops[0]->world();
        }

        auto vars = &world->vars();
        auto muts = &world->muts();
        auto ptr  = ops_ptr();
        gid_      = world->next_gid();

        for (size_t i = 0, e = ops.size(); i != e; ++i) {
            auto op = ops[i];
            ptr[i]  = op;
            dep_ |= op->dep_;
            vars_ = vars->merge(vars_, op->local_vars());
            muts_ = muts->merge(muts_, op->local_muts());
            hash_ = fe::hash_combine(hash_, op->gid());
        }
    }
}

Def::Def(Node n, const Def* type, Defs ops, flags_t flags)
    : Def(nullptr, n, type, ops, flags) {}

Def::Def(Node node, const Def* type, size_t num_ops, flags_t flags)
    : flags_(flags)
    , node_(node)
    , mut_(true)
    , external_(false)
    , annex_(false)
    , dirty_(false)
    , dep_(node2dep(node, true))
    , num_ops_(num_ops)
    , type_(type) {
    gid_  = world().next_gid();
    hash_ = fe::hash(gid());
    var_  = nullptr;
    std::fill_n(ops_ptr(), num_ops, nullptr);
}

Def::Def(Node node, Def* binder)
    : binder_(binder) // the binder is stored here instead of as an op, so a Var stays out of the operand graph
    , flags_(0)
    , node_(node)
    , mut_(false)
    , external_(false)
    , annex_(false)
    , dirty_(false)
    , dep_(node2dep(node, false))
    , num_ops_(0)
    , type_(nullptr) {
    gid_  = binder->world().next_gid();
    vars_ = Vars(as<Var>());
    hash_ = fe::hash_begin(node_t(Node::Var));
    hash_ = fe::hash_combine(hash_, binder->gid());
}

Nat::Nat(World& world)
    : Def(Node, world.type(), Defs{}, 0) {}

UMax::UMax(World& world, Defs ops)
    : Def(Node, world.univ(), ops, 0) {}

/*
 * immutabilize
 */

bool Def::is_immutabilizable() {
    if (!is_set()) return false;

    auto v = has_var();
    for (auto op : deps()) {
        if (v && op->has_free_var(v)) return false;
        for (auto mut : op->local_muts())
            if (mut == this) return false; // recursion
    }
    return true;
}

/*
 * reduce
 */

Defs Def::reduce_(const Def* arg) const {
    if (auto var = has_var()) return world().reduce(var, arg);
    auto off = reduction_offset();
    return {ops().begin() + off, num_ops() - off};
}

/*
 * Def - set
 */

void Def::watch() const {
#ifdef MIM_ENABLE_CHECKS
    if (world().watchpoints().contains(gid())) fe::breakpoint();
#endif
}

Def* Def::finalize() {
    if (auto t = check()->zonk(); t != type()) type_ = t;
    return this;
}

Def* Def::set(Defs ops) {
    watch();
    invalidate();

    size_t n = ops.size();
    assert(n == num_ops() && "num ops don't match");

    for (size_t i = 0; i != n; ++i) {
        auto def = check(i, ops[i]);
        assert(def);
        ops_ptr()[i] = def;
    }
#ifndef NDEBUG
    curr_op_ = n;
#endif

    return finalize();
}

Def* Def::set(size_t i, const Def* def) {
    watch();
    invalidate();
    def = check(i, def);
    assert(def && !op(i) && curr_op_++ == i);
    ops_ptr()[i] = def;

    if (i + 1 == num_ops()) return finalize(); // set last op, so check kind
    return this;
}

Def* Def::set_type(const Def* type) {
    invalidate();
    type_ = type;
    return this;
}

Def* Def::unset() {
    invalidate();
#ifndef NDEBUG
    curr_op_ = 0;
#endif
    std::fill_n(ops_ptr(), num_ops(), nullptr);
    return this;
}

/*
 * free_vars
 */

const Def* Def::var() {
    if (var_) return var_;
    return world().var(this);
}

// clang-format off
const Def* Def::var_type() {
    switch (node()) {
        case Node::Lam:    return as<Lam >()->dom();
        case Node::Pi:     return as<Pi  >()->dom();
        case Node::Rule:   return as<Rule>()->dom();
        case Node::Arr:
        case Node::Pack:   return world().type_idx(arity()); // TODO shapes like (2, 3)
        case Node::Sigma:
        case Node::Join:
        case Node::Meet:   return this;
        case Node::Global:
        case Node::Hole:   return nullptr;
        default:           fe::unreachable();
    }
}
// clang-format on

Vars Def::free_vars() const {
    if (auto mut = isa_mut()) return mut->free_vars();

    auto& vars = world().vars();
    auto fvs   = local_vars();
    for (auto mut : local_muts())
        fvs = vars.merge(fvs, mut->free_vars());

    return fvs;
}

// free_vars() is a union, so any predicate over it distributes over that union: ask f for the local Vars and
// then for each local mutable's free_vars() - instead of merging them all into one throw-away Set.
template<class F>
static bool any_free_vars(const Def* def, F f) {
    if (auto mut = def->isa_mut()) return f(mut->free_vars());

    if (f(def->local_vars())) return true;
    for (auto mut : def->local_muts())
        if (f(mut->free_vars())) return true;

    return false;
}

bool Def::has_free_var(const Var* var) const {
    return any_free_vars(this, [var](Vars fvs) { return fvs.contains(var); });
}

bool Def::has_free_vars() const {
    return any_free_vars(this, [](Vars fvs) { return !fvs.empty(); });
}

bool Def::has_free_vars_in(Vars vars) const {
    return any_free_vars(this, [vars](Vars fvs) { return vars.has_intersection(fvs); });
}

Vars Def::free_vars() {
    if (mark_ == 0) {
        // fixed-point iteration to recompute free vars:
        // (run - 1) identifies the previous iteration; so make sure to offset run by 2 for the first iteration
        auto& w     = world();
        bool cyclic = false;
        w.next_run();
        free_vars<true>(w, cyclic, w.next_run());

        for (bool todo = cyclic; todo;) {
            todo = false;
            free_vars<false>(w, todo, w.next_run());
        }
    }

    return vars_;
}

// w is threaded through instead of re-deriving it via world() - which chases the type chain - at every node.
template<bool init>
Vars Def::free_vars(World& w, bool& todo, u32 run) {
    // If init == true : todo flag detects cycle.
    // If init == false: todo flag keeps track whether sth changed.
    //
    // Recursively recompute free vars. If
    // * mark_ == 0:        Invalid - need to recompute.
    // * mark_ == run - 1:  Previous iteration - need to recompute.
    // * mark_ == run:      We are running in cycles within the *current* iteration of our fixed-point loop.
    // * otherwise:         Valid!
    if (mark_ != 0 && mark_ != run - 1) {
        if constexpr (init) todo |= mark_ == run;
        return vars_;
    }

    mark_ = run;

    auto fvs0  = vars_;
    auto fvs   = fvs0;
    auto& muts = w.muts();
    auto& vars = w.vars();

    for (auto op : deps()) {
        if constexpr (init) fvs = vars.merge(fvs, op->local_vars());

        for (auto mut : op->local_muts()) {
            if constexpr (init) mut->muts_ = muts.insert(mut->muts_, this); // register "this" as user of local_mut
            fvs = vars.merge(fvs, mut->free_vars<init>(w, todo, run));
        }
    }

    if (auto var = has_var()) fvs = vars.erase(fvs, var); // FV(λx.e) = FV(e) \ {x}

    if constexpr (!init) todo |= fvs0 != fvs;

    return vars_ = fvs;
}

void Def::invalidate() {
    if (mark_ != 0) {
        mark_ = 0;
        // TODO optimize if vars empty?
        for (auto mut : users())
            mut->invalidate();
        vars_ = Vars();
        muts_ = Muts();
    }
}

bool Def::is_closed() const {
    bool closed = !has_free_vars();
    assert((!is_external() || closed) && "an external must not have free Vars");
    return closed;
}

Def* Def::outermost_binder() const {
    auto fvs = free_vars();
    if (fvs.empty()) return isa_mut();
    // Terminates: the binder of a free Var of `this` sits strictly further out than `this`.
    return (*fvs.begin())->binder()->outermost_binder();
}

bool Def::nests(Def* mut, MutSet& checked) {
    auto var = has_var(); // `this` is fixed across the recursion, and so is its Var
    auto fvs = mut->free_vars();
    if (fvs.contains(var)) return true;
    if (auto [_, ins] = checked.emplace(mut); !ins) return false;

    for (auto fv : fvs)
        if (this->nests(fv->binder(), checked)) return true;

    return false;
}

bool Def::nests(Def* mut) {
    if (!this->has_var()) return false;
    auto checked = MutSet();
    return this->nests(mut, checked);
}

bool Def::nests(const Def* def) {
    if (auto mut = def->isa_mut()) return this->nests(mut);
    if (!this->has_var()) return false;

    auto checked = MutSet();
    for (auto fv : def->free_vars())
        if (this->nests(fv->binder(), checked)) return true;

    return false;
}

/*
 * Def - misc
 */

Sym Def::sym(const char* s) const { return world().sym(s); }
Sym Def::sym(std::string_view s) const { return world().sym(s); }
Sym Def::sym(std::string s) const { return world().sym(std::move(s)); }

Dbg Def::dbg() const { return world().driver().dbg(dbg_); }
void Def::set_dbg(Dbg d) const { dbg_ = world().driver().dbg(d); }

// Fills in only what is missing (unless @p ow) and interns *once* - the old form went through set(Loc) and
// set(Sym) separately, each re-reading Def::dbg_ and re-interning through the Driver's hash table.
void Def::set_dbg_(Dbg d, bool ow) const {
    if (ow) return set_dbg(d);

    auto mine = dbg(), merged = mine;
    if (!merged.loc()) merged.set(d.loc());
    if (!merged.sym()) merged.set(d.sym());
    if (!(merged == mine)) set_dbg(merged);
}

void Def::set_dbg_key_(DbgKey key, bool ow) const {
    // Nothing of ours to preserve, so adopting the index is exactly what set<Ow>(that Dbg) would compute -
    // but without materialising a Dbg or touching Driver::dbg2idx_ at all.
    if (ow || dbg_ == 0 || dbg_ == key.key_) return void(dbg_ = key.key_);
    set_dbg_(world().driver().dbg(key.key_), false); // rare: we carry a partial Dbg, so merge field-wise
}

const Def* Def::unfold_type() const {
    if (auto t = type()) return t;
    auto& w = world();
    if (auto t = isa<Type>()) return w.type(w.uinc(t->level()));
    assert(isa<Univ>());
    return nullptr;
}

std::string_view Def::node_name() const {
    static constexpr std::string_view Names[Num_Nodes] = {
#define CODE(node, _) #node,
        MIM_NODE(CODE)
#undef CODE
    };
    return Names[node_t(node())];
}

Defs Def::deps() const noexcept {
    // deps() hands out `[type_, op0, op1, ...]` as one contiguous array by stepping back from ops_ptr()
    // (which is `this + 1`). That only holds while type_ occupies the *last* 8 bytes of Def, so moving it
    // - or appending any member after it - would silently corrupt every deps() walk.
    assert((const void*)(ops_ptr() - 1) == (const void*)&type_
           && "Def::type_ must stay Def's last member: Def::deps() and Def::ops_ptr() depend on it");

    // Univ, Type, and Var are the only nodes built without a type - and none of them has deps.
    if (!type_) {
        assert(isa<Univ>() || isa<Type>() || isa<Var>());
        return Defs();
    }

    // Not is_set(): its assertion scans *all* ops and deps() is an inner loop.
    bool set = num_ops_ == 0 || ops_ptr()[num_ops_ - 1];
    return Defs(ops_ptr() - 1, (set ? num_ops_ : 0) + 1);
}

bool Def::is_term() const {
    if (auto t = type())
        if (auto u = t->type())
            if (auto type = u->isa<Type>()) return Lit::isa(type->level()) == nat_t(0);
    return false;
}

#ifndef NDEBUG
const Def* Def::debug_prefix(std::string prefix) const { return set_dbg(dbg().set(sym(prefix + sym().str()))), this; }
const Def* Def::debug_suffix(std::string suffix) const { return set_dbg(dbg().set(sym(sym().str() + suffix))), this; }
#endif

/*
 * cmp
 */

Def::Cmp Def::cmp(const Def* a, const Def* b) {
    if (a == b) return Cmp::E;

    if (a->is_mutable() != b->is_mutable()) return a->is_mutable() ? Cmp::G : Cmp::L;

    // clang-format off
    if (a->node()    != b->node()   ) return a->node()    < b->node()    ? Cmp::L : Cmp::G;
    if (a->num_ops() != b->num_ops()) return a->num_ops() < b->num_ops() ? Cmp::L : Cmp::G;
    if (a->flags()   != b->flags()  ) return a->flags()   < b->flags()   ? Cmp::L : Cmp::G;
    // clang-format on

    if (a->is_mutable()) return Cmp::U; // two distinct mutables of the same shape are incomparable

    if (auto va = a->isa<Var>()) {
        auto vb = b->as<Var>();
        auto ma = va->binder();
        auto mb = vb->binder();
        if (ma->is_set() && ma->free_vars().contains(vb)) return Cmp::L;
        if (mb->is_set() && mb->free_vars().contains(va)) return Cmp::G;
        return Cmp::U;
    }

    // heuristic: iterate backwards as index (often a Lit) comes last and will faster find a solution
    for (size_t i = a->num_ops(); i-- != 0;)
        if (auto res = cmp(a->op(i), b->op(i)); res == Cmp::L || res == Cmp::G) return res;

    return cmp(a->type(), b->type());
}

template<Def::Cmp c>
bool Def::cmp_(const Def* a, const Def* b) {
    auto res = cmp(a, b);
    if (res == Cmp::U) {
        a->world().WLOG("resorting to unstable gid-based compare for commute check");
        return c == Cmp::L ? a->gid() < b->gid() : a->gid() > b->gid();
    }
    return res == c;
}

// clang-format off
bool Def::less   (const Def* a, const Def* b) { return cmp_<Cmp::L>(a, b); }
bool Def::greater(const Def* a, const Def* b) { return cmp_<Cmp::G>(a, b); }
// clang-format on

// clang-format off

/*
 * dispatch
 *
 * All of these used to be `virtual`. Def has no vtable - a vptr would cost 8 bytes on *every* node in the
 * World - so each is one function switching on Def::node() with the former override inlined right here.
 */

const Def* Def::immutabilize() {
    auto& w = world();
    switch (node()) {
        case Node::Pi:    return is_immutabilizable() ? w.pi(as<Pi>()->dom(), as<Pi>()->codom()) : nullptr;
        case Node::Sigma: return is_immutabilizable() ? w.sigma(ops()) : nullptr;
        case Node::Rule:  return nullptr; // TODO should we ever immutabilize Rules?
        case Node::Arr:
        case Node::Pack: {
            auto seq = as<Seq>();
            auto arr = node() == Node::Arr;
            if (is_immutabilizable())
                return arr ? w.arr(seq->arity(), seq->body()) : w.pack(seq->arity(), seq->body());
            // below the threshold an unrollable Seq becomes an explicit Sigma/Tuple
            if (auto n = Lit::isa(seq->arity()); n && *n < w.flags().scalarize_threshold) {
                auto elems = DefVec(*n, [&](size_t i) { return seq->reduce(w.lit_idx(*n, i)); });
                return arr ? w.sigma(elems) : w.tuple(elems);
            }
            return nullptr;
        }
        default: return nullptr;
    }
}

size_t Def::reduction_offset() const noexcept {
    switch (node()) {
        case Node::Join:
        case Node::Lam:
        case Node::Meet:
        case Node::Pack:
        case Node::Sigma: return 0;
        case Node::Arr:
        case Node::Pi:
        case Node::Rule:  return 1;
        default:          return size_t(-1);
    }
}

const Def* Def::arity() const {
    switch (node()) {
        case Node::Arr:   return op(0);
        case Node::Sigma: return num_ops() != 1 || isa_mut() ? world().lit_nat(num_ops()) : op(0)->arity();
        case Node::Pack:
            if (auto arr = type()->isa<Arr>()) return arr->arity();
            return type() == world().sigma() ? world().lit_nat_0() : world().lit_nat_1();
        default:
            if (auto t = type(); t && !t->isa<Type>()) return t->arity();
            return world().lit_nat_1();
    }
}

// clang-format on

void Def::externalize() { return world().externals().externalize(this); }
void Def::internalize() { return world().externals().internalize(this); }

void Def::transfer_external(Def* to) {
    assert(this->sym() == to->sym());
    internalize();
    to->externalize();
}

std::string Def::unique_name() const { return sym().str() + "_"s + std::to_string(gid()); }

nat_t Def::num_tprojs() const {
    if (auto a = Lit::isa(arity()); a && *a < world().flags().scalarize_threshold) return *a;
    return 1;
}

const Def* Def::proj(nat_t a, nat_t i) const {
    if (a == 1) {
        assert(i == 0 && "only inhabitant of Idx 2 is 0_1");
        auto t = type();
        if (!t) return this;
        if (!isa_mut<Sigma>() && !t->isa_mut<Sigma>()) return this;
    }

    if (auto seq = isa<Seq>()) {
        if (seq->has_var()) return seq->reduce(world().lit_idx(a, i));
        return seq->body();
    }

    if (isa<Prod>()) return op(i);

    return world().extract(this, a, i); // only compute world() on the path that needs it
}

/*
 * Idx
 */

const Def* Idx::isa(const Def* def) {
    if (auto app = def->isa<App>()) {
        if (app->callee()->isa<Idx>()) return app->arg();
    }

    return nullptr;
}

std::optional<nat_t> Idx::isa_lit(const Def* def) {
    if (auto size = Idx::isa(def))
        if (auto l = Lit::isa(size)) return l;
    return {};
}

std::optional<nat_t> Idx::size2bitwidth(const Def* size) {
    if (size->isa<Top>()) return 64;
    if (auto s = Lit::isa(size)) return size2bitwidth(*s);
    return {};
}

/*
 * Global
 */

const App* Global::type() const { return Def::type()->as<App>(); }
const Def* Global::alloced_type() const { return type()->arg(0); }

} // namespace mim
