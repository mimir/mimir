#include "mim/def.h"

#include <algorithm>

#include <absl/container/fixed_array.h>
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

Def::Def(World* world, Node node, const Def* type, Defs ops, flags_t flags)
    : world_(world)
    , flags_(flags)
    , node_(node)
    , mut_(false)
    , external_(false)
    , annex_(false)
    , dirty_(false)
    , dep_(node == Node::Hole    ? fe::to_underlying(Dep::Hole)
           : node == Node::Proxy ? fe::to_underlying(Dep::Proxy)
           : node == Node::Var   ? fe::to_underlying(Dep::Var | Dep::Mut)
                                 : 0)
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
    , dep_(fe::to_underlying(Dep::Mut | (node == Node::Hole ? Dep::Hole : Dep::None)))
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
    , dep_(fe::to_underlying(Dep::Var | Dep::Mut))
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

    if (auto v = has_var()) {
        for (auto op : deps())
            if (op->has_free_var(v)) return false;
    }
    for (auto op : deps()) {
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
    return {ops().begin() + reduction_offset(), num_ops() - reduction_offset()};
}

const Def* Def::refine(size_t i, const Def* new_op) const {
    auto new_ops = absl::FixedArray<const Def*>(num_ops());
    for (size_t j = 0, e = num_ops(); j != e; ++j)
        new_ops[j] = i == j ? new_op : op(j);
    return rebuild(type(), new_ops);
}

/*
 * Def - set
 */

Def* Def::set(Defs ops) {
#ifdef MIM_ENABLE_CHECKS
    if (world().watchpoints().contains(gid())) fe::breakpoint();
#endif
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

    if (auto t = check()->zonk(); t != type()) type_ = t;

    return this;
}

Def* Def::set(size_t i, const Def* def) {
#ifdef MIM_ENABLE_CHECKS
    if (world().watchpoints().contains(gid())) fe::breakpoint();
#endif

    invalidate();
    def = check(i, def);
    assert(def && !op(i) && curr_op_++ == i);
    ops_ptr()[i] = def;

    if (i + 1 == num_ops()) { // set last op, so check kind
        if (auto t = check()->zonk(); t != type()) type_ = t;
    }

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
    std::ranges::fill(ops_ptr(), ops_ptr() + num_ops(), nullptr);
    return this;
}

/*
 * free_vars
 */

const Def* Def::var() {
    if (var_) return var_;
    return world().var(this);
}

const Def* Def::var_type() {
    auto& w = world();

    // clang-format off
    if (auto lam  = isa<Lam  >()) return lam->dom();
    if (auto pi   = isa<Pi   >()) return pi ->dom();
    if (auto sig  = isa<Sigma>()) return sig;
    if (auto arr  = isa<Arr  >()) return w.type_idx(arr ->arity()); // TODO shapes like (2, 3)
    if (auto pack = isa<Pack >()) return w.type_idx(pack->arity()); // TODO shapes like (2, 3)
    if (auto rule = isa<Rule >()) return rule->type()->dom();
    if (isa<Bound >()) return this;
    if (isa<Hole  >()) return nullptr;
    if (isa<Global>()) return nullptr;
    // clang-format on
    fe::unreachable();
}

Vars Def::free_vars() const {
    if (auto mut = isa_mut()) return mut->free_vars();

    auto& vars = world().vars();
    auto fvs   = local_vars();
    for (auto mut : local_muts())
        fvs = vars.merge(fvs, mut->free_vars());

    return fvs;
}

bool Def::has_free_var(const Var* var) const {
    if (auto mut = isa_mut()) return mut->free_vars().contains(var);

    if (local_vars().contains(var)) return true;
    for (auto mut : local_muts())
        if (mut->free_vars().contains(var)) return true;

    return false;
}

bool Def::has_free_vars() const {
    if (auto mut = isa_mut()) return !mut->free_vars().empty();

    if (!local_vars().empty()) return true;
    for (auto mut : local_muts())
        if (!mut->free_vars().empty()) return true;

    return false;
}

bool Def::has_free_vars_in(Vars vars) const {
    if (auto mut = isa_mut()) return vars.has_intersection(mut->free_vars());

    if (vars.has_intersection(local_vars())) return true;
    for (auto mut : local_muts())
        if (vars.has_intersection(mut->free_vars())) return true;

    return false;
}

Vars Def::free_vars() {
    if (mark_ == 0) {
        // fixed-point iteration to recompute free vars:
        // (run - 1) identifies the previous iteration; so make sure to offset run by 2 for the first iteration
        auto& w     = world();
        bool cyclic = false;
        w.next_run();
        free_vars<true>(cyclic, w.next_run());

        for (bool todo = cyclic; todo;) {
            todo = false;
            free_vars<false>(todo, w.next_run());
        }
    }

    return vars_;
}

template<bool init>
Vars Def::free_vars(bool& todo, uint32_t run) {
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
    auto& w    = world();
    auto& muts = w.muts();
    auto& vars = w.vars();

    for (auto op : deps()) {
        if constexpr (init) fvs = vars.merge(fvs, op->local_vars());

        for (auto mut : op->local_muts()) {
            if constexpr (init) mut->muts_ = muts.insert(mut->muts_, this); // register "this" as user of local_mut
            fvs = vars.merge(fvs, mut->free_vars<init>(todo, run));
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
    if (local_vars().empty() && local_muts().empty()) return true;
#ifdef MIM_ENABLE_CHECKS
    assert(!is_external() || !has_free_vars());
#endif
    return !has_free_vars();
}

bool Def::is_open() const {
    if (!local_vars().empty()) return true;
    return has_free_vars();
}

Def* Def::outermost_binder() const {
    if (is_closed()) return isa_mut();
    return (*free_vars().begin())->outermost_binder();
}

bool Def::nests(Def* mut, MutSet& checked) {
    if (mut->has_free_var(this->has_var())) return true;
    if (auto [_, ins] = checked.emplace(mut); !ins) return false;

    for (auto fv : mut->free_vars())
        if (this->nests(fv->binder(), checked)) return true;

    return false;
}

bool Def::nests(Def* mut) {
    if (this->has_var()) {
        auto checked = MutSet{};
        return this->nests(mut, checked);
    }
    return false;
}

bool Def::nests(const Def* def) {
    if (auto mut = def->isa_mut()) return this->nests(mut);

    if (has_var()) {
        auto checked = MutSet();
        for (auto fv : def->free_vars())
            if (this->nests(fv->binder(), checked)) return true;
    }
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

void Def::set_loc(Driver& driver, Loc l) const {
    if (auto d = driver.dbg(dbg_); !d.loc()) dbg_ = driver.dbg(d.set(l));
}

const Def* Def::unfold_type() const {
    if (auto t = type()) return t;
    auto& w = world();
    if (auto t = isa<Type>()) return w.type(w.uinc(t->level()));
    assert(isa<Univ>());
    return nullptr;
}

std::string_view Def::node_name() const {
    switch (node()) {
#define CODE(name, _) \
    case Node::name: return #name;
        MIM_NODE(CODE)
#undef CODE
        default: fe::unreachable();
    }
}

Defs Def::deps() const noexcept {
    // deps() hands out `[type_, op0, op1, ...]` as one contiguous array by stepping back from ops_ptr()
    // (which is `this + 1`). That only holds while type_ occupies the *last* 8 bytes of Def, so moving it
    // - or appending any member after it - would silently corrupt every deps() walk.
    assert((const void*)(ops_ptr() - 1) == (const void*)&type_
           && "Def::type_ must stay Def's last member: Def::deps() and Def::ops_ptr() depend on it");

    if (isa<Type>() || isa<Univ>()) return Defs();
    if (isa<Var>()) return Defs(); // the binder lives in binder_, not in an op
    assert(type_);
    return Defs(ops_ptr() - 1, (is_set() ? num_ops_ : 0) + 1);
}

Judge Def::judge() const noexcept {
    switch (node()) {
#define CODE(n, j) \
    case Node::n: return j;
        MIM_NODE(CODE)
#undef CODE
        default: fe::unreachable();
    }
}

bool Def::is_term() const {
    if (auto t = type()) {
        if (auto u = t->type()) {
            if (auto type = u->isa<Type>()) {
                if (auto level = Lit::isa(type->level())) return *level == 0;
            }
        }
    }
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

    if (a->isa_imm() && b->isa_mut()) return Cmp::L;
    if (a->isa_mut() && b->isa_imm()) return Cmp::G;

    // clang-format off
    if (a->node()    != b->node()   ) return a->node()    < b->node()    ? Cmp::L : Cmp::G;
    if (a->num_ops() != b->num_ops()) return a->num_ops() < b->num_ops() ? Cmp::L : Cmp::G;
    if (a->flags()   != b->flags()  ) return a->flags()   < b->flags()   ? Cmp::L : Cmp::G;
    // clang-format on

    if (a->isa_mut() && b->isa_mut()) return Cmp::U;
    assert(a->isa_imm() && b->isa_imm());

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

const Def* Def::rebuild_(World& w, const Def* t, Defs o) const {
    switch (node()) {
        case Node::App:     return w.app(o[0], o[1]);
        case Node::Arr:     return w.arr(o[0], o[1]);
        case Node::Bot:     return w.bot(t)->set(dbg());
        case Node::Extract: return w.extract(o[0], o[1]);
        case Node::Idx:     return w.type_idx();
        case Node::Inj:     return w.inj(t, o[0])->set(dbg());
        case Node::Insert:  return w.insert(o[0], o[1], o[2]);
        case Node::Join:    return w.join(o)->set(dbg());
        case Node::Lam:     return w.lam(t->as<Pi>(), o[0], o[1]);
        case Node::Lit:     return w.lit(t, as<Lit>()->get());
        case Node::Match:   return w.match(o);
        case Node::Meet:    return w.meet(o)->set(dbg());
        case Node::Merge:   return w.merge(t, o);
        case Node::Nat:     return w.type_nat();
        case Node::Pack:    return w.pack(t->arity(), o[0]);
        case Node::Pi:      return w.pi(o[0], o[1], as<Pi>()->is_implicit());
        case Node::Proxy:   return w.proxy(t, o, as<Proxy>()->tag());
        case Node::Reform:  return w.reform(o[0]);
        case Node::Rule:    return w.rule(t->as<Reform>(), o[0], o[1], o[2]);
        case Node::Sigma:   return w.sigma(o);
        case Node::Split:   return w.split(t, o[0]);
        case Node::Top:     return w.top(t)->set(dbg());
        case Node::Tuple:   return w.tuple(t, o);
        case Node::Type:    return w.type(o[0]);
        case Node::UInc:    return w.uinc(o[0], as<UInc>()->offset());
        case Node::UMax:    return w.umax(o);
        case Node::Uniq:    return w.uniq(o[0]);
        case Node::Univ:    return w.univ();
        case Node::Global:  fe::unreachable(); // *mutable*: rebuilt via Def::stub_
        case Node::Hole:    fe::unreachable(); // *mutable*: rebuilt via Def::stub_
        case Node::Var:     fe::unreachable(); // binder is in binder_, not an op; see Rewriter::rewrite_imm_Var
        case Node::Axm: {
            auto axm = as<Axm>();
            if (&w != &world())
                return w.axm(axm->normalizer(), axm->curry(), axm->trip(), t, axm->plugin(), axm->tag(), axm->sub())
                    ->set(dbg());
            assert(Checker::alpha<Checker::Check>(t, type()));
            return this;
        }
    }
    fe::unreachable();
}

Def* Def::stub_(World& w, const Def* t) {
    switch (node()) {
        case Node::Arr:    return w.mut_arr  (t);
        case Node::Global: return w.global   (t, as<Global>()->is_mutable());
        case Node::Hole:   return w.mut_hole (t);
        case Node::Lam:    return w.mut_lam  (t->as<Pi>());
        case Node::Pack:   return w.mut_pack (t);
        case Node::Pi:     return w.mut_pi   (t, as<Pi>()->is_implicit());
        case Node::Rule:   return w.mut_rule (t->as<Reform>());
        case Node::Sigma:  return w.mut_sigma(t, num_ops());
        default:           fe::unreachable(); // only *mutables* have a stub
    }
}

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
        if (!type()) return this;
        if (!isa_mut<Sigma>() && !type()->isa_mut<Sigma>()) return this;
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
