#pragma once

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <span>

#include <fe/assert.h>
#include <fe/cast.h>
#include <fe/enum.h>

#include "mim/config.h"

#include "mim/util/dbg.h"
#include "mim/util/sets.h"
#include "mim/util/util.h"
#include "mim/util/vector.h"

// clang-format off
#define MIM_NODE(X)                                                                                                \
    X(Lit,    Judge::Intro) /* keep this first - causes Lit to appear left in Def::less/Def::greater*/             \
    X(Axm,    Judge::Intro)                                                                                        \
    X(Var,    Judge::Intro)                                                                                        \
    X(Global, Judge::Intro)                                                                                        \
    X(Proxy,  Judge::Intro)                                                                                        \
    X(Hole,   Judge::Hole )                                                                                        \
    X(Type,   Judge::Meta ) X(Univ,  Judge::Meta ) X(UMax,    Judge::Meta) X(UInc,   (Judge::Meta               )) \
    X(Pi,     Judge::Form ) X(Lam,   Judge::Intro) X(App,     Judge::Elim)                                         \
    X(Sigma,  Judge::Form ) X(Tuple, Judge::Intro) X(Extract, Judge::Elim) X(Insert, (Judge::Intro | Judge::Elim)) \
    X(Arr,    Judge::Form ) X(Pack,  Judge::Intro)                                                                 \
    X(Join,   Judge::Form ) X(Inj,   Judge::Intro) X(Match,   Judge::Elim) X(Top,    (Judge::Intro              )) \
    X(Meet,   Judge::Form ) X(Merge, Judge::Intro) X(Split,   Judge::Elim) X(Bot,    (Judge::Intro              )) \
    X(Reform, Judge::Form ) X(Rule,  Judge::Intro)                                                                 \
    X(Uniq,   Judge::Form )                                                                                        \
    X(Nat,    Judge::Form )                                                                                        \
    X(Idx,    Judge::Intro)

#define MIM_IMM_NODE(X)                                                                                            \
    X(Lit)                                                                                                         \
    X(Axm)                                                                                                         \
    X(Var)                                                                                                         \
    X(Proxy)                                                                                                       \
    X(Type)   X(Univ)  X(UMax)    X(UInc)                                                                          \
    X(Pi)     X(Lam)   X(App)                                                                                      \
    X(Sigma)  X(Tuple) X(Extract) X(Insert)                                                                        \
    X(Arr)    X(Pack)                                                                                              \
    X(Join)   X(Inj)   X(Match)   X(Top)                                                                           \
    X(Meet)   X(Merge) X(Split)   X(Bot)                                                                           \
    X(Reform) X(Rule)                                                                                              \
    X(Uniq)                                                                                                        \
    X(Nat)                                                                                                         \
    X(Idx)

#define MIM_MUT_NODE(X)                                                                                            \
    X(Global)                                                                                                      \
    X(Hole)                                                                                                        \
    X(Pi)    X(Lam)                                                                                                \
    X(Sigma)                                                                                                       \
    X(Arr)   X(Pack)                                                                                               \
    X(Rule)
// clang-format on

namespace mim {

class App;
class Axm;
class Var;
class Def;
class World;

/// @name Def
/// GIDSet / GIDMap keyed by Def::gid of `const Def*`.
///@{
template<class To>
using DefMap  = GIDMap<const Def*, To>;
using DefSet  = GIDSet<const Def*>;
using Def2Def = DefMap<const Def*>;
using Defs    = View<const Def*>;
using DefVec  = Vector<const Def*>;
///@}

/// @name Def (Mutable)
/// GIDSet / GIDMap keyed by Def::gid of `Def*`.
///@{
template<class To>
using MutMap  = GIDMap<Def*, To>;
using MutSet  = GIDSet<Def*>;
using Mut2Mut = MutMap<Def*>;
using Muts    = Sets<Def>::Set;
///@}

/// @name Var
/// GIDSet / GIDMap keyed by Var::gid of `const Var*`.
///@{
template<class To>
using VarMap  = GIDMap<const Var*, To>;
using VarSet  = GIDSet<const Var*>;
using Var2Var = VarMap<const Var*>;
using Vars    = Sets<const Var>::Set;
///@}

using NormalizeFn = const Def* (*)(const Def*, const Def*, const Def*);

/// @name Enums that classify certain aspects of Def%s.
///@{

enum class Node : node_t {
#define CODE(node, _) node,
    MIM_NODE(CODE)
#undef CODE
};

#define CODE(node, _) +size_t(1)
static constexpr size_t Num_Nodes = size_t(0) MIM_NODE(CODE);
#undef CODE

/// Tracks whether a Def transitively depends - through its Def::deps() but only up to (and excluding) the next
/// *mutable* - on certain kinds of Def%s.
/// @see Def::has_dep
enum class Dep : unsigned {
    None  = 0,      ///< Depends on nothing of interest.
    Mut   = 1 << 0, ///< Depends on a *mutable*.
    Var   = 1 << 1, ///< Depends on a Var.
    Hole  = 1 << 2, ///< Depends on a Hole.
    Proxy = 1 << 3, ///< Depends on a Proxy.
};

/// [Judgement](https://ncatlab.org/nlab/show/judgment).
enum class Judge : u32 {
    // clang-format off
    Form  = 1 << 0, ///< [Type Formation](https://ncatlab.org/nlab/show/type+formation) like `T -> T`.
    Intro = 1 << 1, ///< [Term Introduction](https://ncatlab.org/nlab/show/natural+deduction) like `λ(x: Nat): Nat = x`.
    Elim  = 1 << 2, ///< [Term Elimination](https://ncatlab.org/nlab/show/term+elimination) like `f a`.
    Meta  = 1 << 3, ///< Meta rules for Univ%erse and Type levels.
    Hole  = 1 << 4, ///< Special rule for Hole.
    // clang-format on
};

/// Classifies whether a [`Node`](@ref mim::Node) may occur as a *mutable*, an *immutable*, or both.
/// @see @ref mut
enum class Mut {
    // clang-format off
    Mut = 1 << 0, ///< Node may be mutable.
    Imm = 1 << 1, ///< Node may be immutable.
    // clang-format on
};
///@}

} // namespace mim

#ifndef DOXYGEN
// clang-format off
template<> struct fe::is_bit_enum<mim::Dep>   : std::true_type {};
template<> struct fe::is_bit_enum<mim::Judge> : std::true_type {};
template<> struct fe::is_bit_enum<mim::Mut>   : std::true_type {};
// clang-format on
#endif

namespace mim {

/// Use as mixin to wrap all kind of Def::proj and Def::projs variants.
#define MIM_PROJ(NAME, CONST)                                                                     \
    nat_t num_##NAME##s() CONST noexcept { return ((const Def*)NAME())->num_projs(); }            \
    nat_t num_t##NAME##s() CONST noexcept { return ((const Def*)NAME())->num_tprojs(); }          \
    const Def* NAME(nat_t a, nat_t i) CONST noexcept { return ((const Def*)NAME())->proj(a, i); } \
    const Def* NAME(nat_t i) CONST noexcept { return ((const Def*)NAME())->proj(i); }             \
    const Def* t##NAME(nat_t i) CONST noexcept { return ((const Def*)NAME())->tproj(i); }         \
    template<nat_t A = std::dynamic_extent, class F>                                              \
    auto NAME##s(F f) CONST noexcept {                                                            \
        return ((const Def*)NAME())->projs<A, F>(f);                                              \
    }                                                                                             \
    template<class F>                                                                             \
    auto t##NAME##s(F f) CONST noexcept {                                                         \
        return ((const Def*)NAME())->tprojs<F>(f);                                                \
    }                                                                                             \
    template<nat_t A = std::dynamic_extent>                                                       \
    auto NAME##s() CONST noexcept {                                                               \
        return ((const Def*)NAME())->projs<A>();                                                  \
    }                                                                                             \
    auto t##NAME##s() CONST noexcept { return ((const Def*)NAME())->tprojs(); }                   \
    template<class F>                                                                             \
    auto NAME##s(nat_t a, F f) CONST noexcept {                                                   \
        return ((const Def*)NAME())->projs<F>(a, f);                                              \
    }                                                                                             \
    auto NAME##s(nat_t a) CONST noexcept { return ((const Def*)NAME())->projs(a); }

/// CRTP-based mixin to declare setters for Def::loc \& Def::name using a *covariant* return type.
template<class P, class D = Def>
class // D is only needed to make the resolution `D::template set` lazy
#ifdef _MSC_VER
    __declspec(empty_bases)
#endif
    Setters {
private:
    P* super() { return static_cast<P*>(this); }
    const P* super() const { return static_cast<const P*>(this); }

public:
    // clang-format off
    template<bool Ow = false> const P* set(Loc l               ) const { super()->D::template set<Ow>(l); return super(); }
    template<bool Ow = false>       P* set(Loc l               )       { super()->D::template set<Ow>(l); return super(); }
    template<bool Ow = false> const P* set(       Sym s        ) const { super()->D::template set<Ow>(s); return super(); }
    template<bool Ow = false>       P* set(       Sym s        )       { super()->D::template set<Ow>(s); return super(); }
    template<bool Ow = false> const P* set(       std::string s) const { super()->D::template set<Ow>(std::move(s)); return super(); }
    template<bool Ow = false>       P* set(       std::string s)       { super()->D::template set<Ow>(std::move(s)); return super(); }
    template<bool Ow = false> const P* set(Loc l, Sym s        ) const { super()->D::template set<Ow>(l, s); return super(); }
    template<bool Ow = false>       P* set(Loc l, Sym s        )       { super()->D::template set<Ow>(l, s); return super(); }
    template<bool Ow = false> const P* set(Loc l, std::string s) const { super()->D::template set<Ow>(l, std::move(s)); return super(); }
    template<bool Ow = false>       P* set(Loc l, std::string s)       { super()->D::template set<Ow>(l, std::move(s)); return super(); }
    template<bool Ow = false> const P* set(Dbg d               ) const { super()->D::template set<Ow>(d); return super(); }
    template<bool Ow = false>       P* set(Dbg d               )       { super()->D::template set<Ow>(d); return super(); }
    // clang-format on
};

/// Options for Def::dot and World::dot.
/// @note Def::dot and World::dot honor DotConfig::max; World::dot also honors DotConfig::all_annexes.
struct DotConfig {
    int max             = std::numeric_limits<int>::max(); ///< Maximum recursion depth.
    bool all_annexes    = false;                           ///< Include all annexes - even if unused (World::dot only).
    bool follow_types   = false;                           ///< Follow Def::type() dependencies.
    bool inline_consts  = false; ///< Wire up literals, axioms, etc. with normal edges instead of detaching them.
    bool default_filter = false; ///< Show Lam::filter() even if it has its default value.
    bool show_hidden    = false; ///< Render otherwise-transparent detached edges (Var→binder back-edges,
                                 ///< shared literals/axioms, type edges) with a visible color.
};

/// Base class for all Def%s.
///
/// These are the most important subclasses:
/// | Type Formation    | Term Introduction | Term Elimination  |
/// | ----------------- | ----------------- | ----------------- |
/// | Pi                | Lam               | App               |
/// | Sigma / Arr       | Tuple / Pack      | Extract           |
/// |                   | Insert            | Insert            |
/// | Uniq              |                   |                   |
/// | Join              | Inj               | Match             |
/// | Meet              | Merge             | Split             |
/// | Reform            | Rule              |                   |
/// | Nat               | Lit               |                   |
/// | Idx               | Lit               |                   |
/// In addition there is:
/// * Var: A variable. Currently the following Def%s may be binders:
///     * Pi, Lam, Sigma, Arr, Pack
/// * Axm: To introduce new entities.
/// * Proxy: Used for intermediate values during optimizations.
/// * Hole: A metavariable filled in by the type inference (always mutable as holes are filled in later).
/// * Type, Univ, UMax, UInc: To keep track of type levels.
///
/// The data layout (see World::alloc and Def::deps) looks like this:
/// ```
/// Def| type | op(0) ... op(num_ops-1) |
///           |-----------ops-----------|
///                  deps
///    |--------------------------------| if type() != nullptr &&  is_set()
///           |-------------------------| if type() == nullptr &&  is_set()
///    |------|                           if type() != nullptr && !is_set()
///    ||                                 if type() == nullptr && !is_set()
/// ```
/// @attention This means that any subclass of Def **must not** introduce additional members.
/// @see @ref mut
class Def : public fe::RuntimeCast<Def> {
private:
    Def& operator=(const Def&) = delete;
    Def(const Def&)            = delete;
    Def(Def&&)                 = delete;

protected:
    /// @name C'tors and D'tors
    ///@{
    Def(World*, Node, const Def* type, Defs ops, flags_t flags); ///< Constructor for an *immutable* Def.
    Def(Node, const Def* type, Defs ops, flags_t flags);         ///< As above but World retrieved from @p type.
    Def(Node, const Def* type, size_t num_ops, flags_t flags);   ///< Constructor for a *mutable* Def.
    Def(Node, Def* binder);                                      ///< Constructor for a Var; stores its @p binder.
    virtual ~Def() = default;
    ///@}

public:
    /// @name Getters
    ///@{
    World& world() const noexcept;
    constexpr flags_t flags() const noexcept { return flags_; }
    constexpr u32 gid() const noexcept { return gid_; }   ///< Global id - *unique* number for this Def.
    constexpr u32 tid() const noexcept { return tid_; }   ///< Trie id - only used in Trie.
    constexpr u32 mark() const noexcept { return mark_; } ///< Used internally by free_vars().
    constexpr size_t hash() const noexcept { return hash_; }
    constexpr Node node() const noexcept { return node_; }
    std::string_view node_name() const;
    ///@}

    /// @name Judgement
    /// What kind of Judge%ment represents this Def?
    ///@{
    Judge judge() const noexcept;
    // clang-format off
    bool is_form()  const noexcept { return fe::has_flag(judge(), Judge::Form);  }
    bool is_intro() const noexcept { return fe::has_flag(judge(), Judge::Intro); }
    bool is_elim()  const noexcept { return fe::has_flag(judge(), Judge::Elim);  }
    bool is_meta()  const noexcept { return fe::has_flag(judge(), Judge::Meta);  }
    // clang-format on
    ///@}

    /// @name type
    ///@{

    /// Yields the "raw" type of this Def (maybe `nullptr`).
    /// @see Def::unfold_type.
    const Def* type() const noexcept;
    /// Yields the type of this Def and builds a new `Type (UInc n)` if necessary.
    const Def* unfold_type() const;
    bool is_term() const;             ///< Is this Def a *term*, i.e. is its type() a Type?
    virtual const Def* arity() const; ///< Number of elements available to Extract / Insert (may be dynamic).
    ///@}

    /// @name ops
    ///@{
    template<size_t N = std::dynamic_extent>
    constexpr auto ops() const noexcept {
        return View<const Def*, N>(ops_ptr(), num_ops_);
    }
    const Def* op(size_t i) const noexcept { return ops()[i]; }
    constexpr size_t num_ops() const noexcept { return num_ops_; }
    ///@}

    /// @name Setting Ops (Mutables Only)
    /// @anchor set_ops
    /// You can set and change the Def::ops of a mutable after construction.
    /// However, you have to obey the following rules:
    /// If Def::is_set() is ...
    ///     * `false`, [set](@ref Def::set) the [operands](@ref Def::ops) from left to right.
    ///     * `true`, Def::unset() the operands first and then start over:
    ///       ```
    ///       mut->unset()->set({a, b, c});
    ///       ```
    ///
    /// MimIR assumes that a mutable is *final*, when its last operand is set.
    /// Then, Def::check() will be invoked.
    ///@{
    /// Yields `true` if empty or the last op is set.
    bool is_set() const {
        if (num_ops() == 0) return true;
        bool result = ops().back();
        assert((!result || std::ranges::all_of(ops().rsubspan(1), [](auto op) { return op; }))
               && "the last operand is set but others in front of it aren't");
        return result;
    }
    Def* set(size_t i, const Def*); ///< Successively set from left to right.
    Def* set(Defs ops);             ///< Set @p ops all at once (no Def::unset necessary beforehand).
    Def* unset();                   ///< Unsets all Def::ops; works even, if not set at all or only partially set.

    /// Update type.
    /// @warning Only make type-preserving updates such as removing Hole%s.
    /// Do this even before updating all other ops()!
    Def* set_type(const Def*);
    ///@}

    /// @name deps
    /// All *dependencies* of a Def and includes:
    /// * Def::type() (if not `nullptr`) and
    /// * the other Def::ops() (only included, if Def::is_set()) in this order.
    ///@{
    Defs deps() const noexcept;
    const Def* dep(size_t i) const noexcept { return deps()[i]; }
    size_t num_deps() const noexcept { return deps().size(); }
    ///@}

    /// @name has_dep
    /// Checks whether one Def::deps() contains specific elements defined in Dep.
    /// This works up to the next *mutable*.
    /// For example, consider the Tuple `tup`: `(?, lam (x: Nat) = y)`:
    /// ```
    /// bool has_hole = tup->has_dep(Dep::Hole); // true
    /// bool has_mut  = tup->has_dep(Dep::Mut);  // true
    /// bool has_var  = tup->has_dep(Dep::Var);  // false - y is contained in another mutable
    /// ```
    ///@{
    Dep dep() const noexcept { return Dep(dep_); }
    bool has_dep() const noexcept { return dep_ != 0; }
    bool has_dep(Dep d) const noexcept { return fe::has_flag(dep(), d); }
    ///@}

    /// @name proj
    /// @anchor proj
    /// Splits this Def via Extract%s or directly accessing the Def::ops in the case of Sigma%s or Arr%ays.
    /// ```
    /// std::array<const Def*, 2> ab = def->projs<2>();
    /// std::array<u64, 2>        xy = def->projs<2>([](auto def) { return Lit::as(def); });
    /// auto [a, b]                  = def->projs<2>();
    /// auto [x, y]                  = def->projs<2>([](auto def) { return Lit::as(def); });
    /// Vector<const Def*> projs1    = def->projs(); // "projs1" has def->num_projs() many elements
    /// Vector<const Def*> projs2    = def->projs(n);// "projs2" has n elements - asserts if incorrect
    /// // same as above but applies Lit::as<nat_t>(def) to each element
    /// Vector<const Lit*> lits1     = def->projs(   [](auto def) { return Lit::as(def); });
    /// Vector<const Lit*> lits2     = def->projs(n, [](auto def) { return Lit::as(def); });
    /// ```
    ///@{

    /// Yields Def::arity(), if it is a Lit, or `1` otherwise.
    nat_t num_projs() const;
    nat_t num_tprojs() const; ///< As above but yields 1, if Flags::scalarize_threshold is exceeded.

    /// Similar to World::extract while assuming an arity of @p a, but also works on Sigma%s and Arr%ays.
    const Def* proj(nat_t a, nat_t i) const;
    const Def* proj(nat_t i) const { return proj(num_projs(), i); }   ///< As above but takes Def::num_projs as arity.
    const Def* tproj(nat_t i) const { return proj(num_tprojs(), i); } ///< As above but takes Def::num_tprojs.

    /// Splits this Def via Def::proj%ections into an Array (if `A == std::dynamic_extent`) or `std::array` (otherwise).
    /// Applies @p f to each element.
    template<nat_t A = std::dynamic_extent, class F>
    auto projs(F f) const {
        using R = std::decay_t<decltype(f(this))>;
        if constexpr (A == std::dynamic_extent) {
            return projs(num_projs(), f);
        } else {
            std::array<R, A> array;
            for (nat_t i = 0; i != A; ++i)
                array[i] = f(proj(A, i));
            return array;
        }
    }

    template<class F>
    auto tprojs(F f) const {
        return projs(num_tprojs(), f);
    }

    template<class F>
    auto projs(nat_t a, F f) const {
        using R = std::decay_t<decltype(f(this))>;
        return Vector<R>(a, [&](nat_t i) { return f(proj(a, i)); });
    }
    template<nat_t A = std::dynamic_extent>
    auto projs() const {
        return projs<A>([](const Def* def) { return def; });
    }
    auto tprojs() const {
        return tprojs([](const Def* def) { return def; });
    }
    auto projs(nat_t a) const {
        return projs(a, [](const Def* def) { return def; });
    }
    ///@}

    /// @name var
    /// @anchor var
    /// Retrieve Var for *mutables*.
    /// @see @ref proj
    ///@{
    MIM_PROJ(var, )
    const Def* var();      ///< Not necessarily a Var: E.g., if the return type is `[]`, this will yield `()`.
    const Def* var_type(); ///< If `this` is a binder, compute the type of its Var%iable.

    const Var* has_var() { return var_; } ///< Only returns not `nullptr`, if Var of this mutable has ever been created.
    /// As above if `this` is a *mutable*.
    const Var* has_var() const {
        if (auto mut = isa_mut()) return mut->has_var();
        return nullptr;
    }

    /// Is `this` a mutable that introduces a Var?
    /// @returns `{nullptr, nullptr}` otherwise.
    template<class D = Def>
    std::pair<D*, const Var*> isa_binder() const {
        if (auto mut = isa_mut<D>()) {
            if (auto var = mut->has_var()) return {mut, var};
        }
        return {nullptr, nullptr};
    }
    ///@}

    /// @name Free Vars and Muts
    /// MimIR splits the free-variable analysis into a *local* and a *global* layer:
    /// * local_muts() / local_vars() only look at the *immutable* fan-out and are cheap, cached, and hash-consed.
    /// * free_vars() close over the *mutable* boundary as well and are the actual set of free Var%s.
    ///   They are computed on demand via a fixed-point iteration and cached in mutables.
    ///   Mutating a mutable transitively invalidates these caches by following users().
    ///@{

    /// Mutables reachable by following *immutable* deps(); `mut->local_muts()` is by definition the set `{ mut }`.
    Muts local_muts() const {
        if (auto mut = isa_mut()) return Muts(mut);
        return muts_;
    }

    /// Var%s reachable by following *immutable* deps().
    /// @note `var->local_vars()` is by definition the set `{ var }`.
    Vars local_vars() const { return mut_ ? Vars() : vars_; }

    /// Global set of free Var%s: extends local_vars() by transitively following *mutables* as well.
    /// @note On a *mutable* this simply forwards to the caching non-`const` overload below.
    Vars free_vars() const;
    Vars free_vars();              ///< As above but drives (and caches) the fixed-point iteration for *mutables*.
    Muts users() { return muts_; } ///< Set of mutables where this mutable is locally referenced.
    bool is_open() const;          ///< Has free_vars()?
    bool is_closed() const;        ///< Has no free_vars()?

    /// @name free_vars predicates
    /// `free_vars()` of an *immutable* is **not** cached: it merges `free_vars()` of every local_muts() entry on
    /// every call, and each Sets::merge allocates, sorts, hashes, and probes the pool.
    /// Since free_vars() is a union, any predicate over it distributes over that union - so these answer the
    /// question without ever materializing the merged set.
    /// Prefer them over `free_vars().contains(...)` / `.empty()` / `has_intersection(...)`.
    ///@{
    bool has_free_var(const Var*) const; ///< Same as `free_vars().contains(var)`.
    bool has_free_vars() const;          ///< Same as `!free_vars().empty()`.
    bool has_free_vars_in(Vars) const;   ///< Same as `vars.has_intersection(free_vars())`.
    ///@}

    /// Transitively walks up free_vars() till the outermoust binder has been found.
    /// @returns `nullptr`, if is_closed() and not a mutable.
    Def* outermost_binder() const;

    /// Does @p this nest @p mut?
    /// The relation is strict: `f->nests(f)` is `false`.
    bool nests(Def* mut);
    /// Does @p this nest @p def?
    /// Also strict: a @p def that only uses @p this%'s own Var sits at @p this%'s level and is *not* nested.
    bool nests(const Def* def);
    ///@}

    /// @name external
    ///@{
    bool is_external() const noexcept { return external_; }
    void externalize();
    void internalize();
    void transfer_external(Def* to);
    bool is_annex() const noexcept { return annex_; }
    ///@}

    /// @name dirty
    /// Scratch bit for Phase%s to mark muts that need re-examination.
    /// @see Phase::taint
    ///@{
    bool is_dirty() const noexcept { return dirty_; }
    void dirty(bool dirty = true) noexcept { dirty_ = dirty; }
    ///@}

    /// @name Casts
    /// @see @ref cast_builtin
    ///@{
    bool is_mutable() const noexcept { return mut_; }

    // clang-format off
    template<class T = Def> const T* isa_imm() const { return isa_mut<T, true>(); }
    template<class T = Def> const T*  as_imm() const { return  as_mut<T, true>(); }
    // clang-format on

    /// If `this` is *mutable*, it will cast `const`ness away and perform a `dynamic_cast` to @p T.
    template<class T = Def, bool invert = false>
    T* isa_mut() const {
        if constexpr (std::is_same<T, Def>::value)
            return mut_ ^ invert ? const_cast<Def*>(this) : nullptr;
        else
            return mut_ ^ invert ? const_cast<Def*>(this)->template isa<T>() : nullptr;
    }

    /// Asserts that `this` is a *mutable*, casts `const`ness away and performs a `static_cast` to @p T.
    template<class T = Def, bool invert = false>
    T* as_mut() const {
        assert(mut_ ^ invert);
        if constexpr (std::is_same<T, Def>::value)
            return const_cast<Def*>(this);
        else
            return const_cast<Def*>(this)->template as<T>();
    }

    /// Like Def::as_mut but - instead of merely asserting in `Debug` builds - throws via fe::throwf when the cast
    /// fails; the mutable counterpart of fe::RuntimeCast::expect (which Def inherits for the general case).
    /// @p fmt / @p args describe what was expected; a plain string works, as does a format string plus arguments.
    template<class T = Def, class... Args>
    T* expect_mut(std::format_string<Args...> fmt, Args&&... args) const {
        if (auto res = isa_mut<T>()) return res;
        fe::throwf("expected {}, but got '{}'", std::format(fmt, std::forward<Args>(args)...), this);
    }
    ///@}

    /// @name Dbg Getters
    ///@{
    Dbg dbg() const { return dbg_; }
    Loc loc() const { return dbg_.loc(); }
    Sym sym() const { return dbg_.sym(); }
    std::string unique_name() const; ///< name + "_" + Def::gid
    ///@}

    /// @name Dbg Setters
    /// Every subclass `S` of Def has the same setters that return `S*`/`const S*` via the mixin Setters.
    ///@{
    // clang-format off
    template<bool Ow = false> const Def* set(Loc l) const { if (Ow || !dbg_.loc()) dbg_.set(l); return this; }
    template<bool Ow = false>       Def* set(Loc l)       { if (Ow || !dbg_.loc()) dbg_.set(l); return this; }
    template<bool Ow = false> const Def* set(Sym s) const { if (Ow || !dbg_.sym()) dbg_.set(s); return this; }
    template<bool Ow = false>       Def* set(Sym s)       { if (Ow || !dbg_.sym()) dbg_.set(s); return this; }
    template<bool Ow = false> const Def* set(       std::string s) const { set<Ow>(sym(std::move(s))); return this; }
    template<bool Ow = false>       Def* set(       std::string s)       { set<Ow>(sym(std::move(s))); return this; }
    template<bool Ow = false> const Def* set(Loc l, Sym s        ) const { set<Ow>(l); set<Ow>(s); return this; }
    template<bool Ow = false>       Def* set(Loc l, Sym s        )       { set<Ow>(l); set<Ow>(s); return this; }
    template<bool Ow = false> const Def* set(Loc l, std::string s) const { set<Ow>(l); set<Ow>(sym(std::move(s))); return this; }
    template<bool Ow = false>       Def* set(Loc l, std::string s)       { set<Ow>(l); set<Ow>(sym(std::move(s))); return this; }
    template<bool Ow = false> const Def* set(Dbg d) const { set<Ow>(d.loc(), d.sym()); return this; }
    template<bool Ow = false>       Def* set(Dbg d)       { set<Ow>(d.loc(), d.sym()); return this; }
    // clang-format on
    ///@}

    /// @name debug_prefix/suffix
    /// Prepends/Appends a prefix/suffix to Def::name - but only in `Debug` build.
    ///@{
#ifndef NDEBUG
    const Def* debug_prefix(std::string) const;
    const Def* debug_suffix(std::string) const;
#else
    const Def* debug_prefix(std::string) const { return this; }
    const Def* debug_suffix(std::string) const { return this; }
#endif
    ///@}

    /// @name Rebuild
    ///@{
    Def* stub(World& w, const Def* type) { return stub_(w, type)->set(dbg()); }
    Def* stub(const Def* type) { return stub(world(), type); }

    /// Def::rebuild%s this Def while using @p new_op as substitute for its @p i'th Def::op
    const Def* rebuild(World& w, const Def* type, Defs ops) const {
        assert(isa_imm());
        return rebuild_(w, type, ops)->set(dbg());
    }
    const Def* rebuild(const Def* type, Defs ops) const { return rebuild(world(), type, ops); }

    /// Tries to make an immutable from a mutable.
    /// This usually works if the mutable isn't recursive and its var isn't used.
    virtual const Def* immutabilize() { return nullptr; }
    bool is_immutabilizable();

    const Def* refine(size_t i, const Def* new_op) const;

    /// @see World::reduce
    template<size_t N = std::dynamic_extent>
    constexpr auto reduce(const Def* arg) const {
        return reduce_(arg).span<N>();
    }

    /// First Def::op that needs to be dealt with during reduction; e.g. for a Pi we don't reduce the Pi::dom.
    /// @see World::reduce
    virtual constexpr size_t reduction_offset() const noexcept { return size_t(-1); }
    ///@}

    /// @name Type Checking
    ///@{

    /// Checks whether the `i`th operand can be set to `def`.
    /// The method returns a possibly updated version of `def` (e.g. where Hole%s have been resolved).
    /// This is the actual `def` that will be set as the `i`th operand.
    virtual const Def* check([[maybe_unused]] size_t i, const Def* def) { return def; }

    /// After all Def::ops have been Def::set, this method will be invoked to check the type of this mutable.
    /// The method returns a possibly updated version of its type (e.g. where Hole%s have been resolved).
    /// If different from Def::type, it will update its Def::type to a Def::zonk%ed version of that.
    virtual const Def* check() { return type(); }

    /// Yields `true`, if Def::local_muts() contain a Hole that is set.
    /// Rewriting (Def::zonk%ing) will resolve the Hole to its operand.
    bool needs_zonk() const;

    /// If Hole%s have been filled, reconstruct the program without them.
    /// Only goes up to but excluding other mutables.
    /// @see https://stackoverflow.com/questions/31889048/what-does-the-ghc-source-mean-by-zonk
    const Def* zonk() const;

    /// If *mutable*, zonk()%s all ops and tries to immutabilize it; otherwise just zonk.
    const Def* zonk_mut() const;
    ///@}

    /// zonk%s all @p defs and returns a new DefVec.
    static DefVec zonk(Defs defs);

    /// @name dump
    /// @note While this output uses Mim syntax, it does usually **not** produce programs that can be read back.
    /// It uses an unscheduled visiting algorithm, and is only meant for debugging purposes.
    ///@{
    void dump() const;
    void dump(int max) const;
    void write(int max) const;
    void write(int max, const char* file) const;
    std::ostream& stream(std::ostream&, int max) const;
    ///@}

    /// @name Syntactic Comparison
    /// Establishes an arbitrary but deterministic total order on Def%s that is stable across runs.
    ///@{
    enum class Cmp {
        L, ///< Less
        G, ///< Greater
        E, ///< Equal
        U, ///< Unknown
    };
    [[nodiscard]] static Cmp cmp(const Def* a, const Def* b);
    [[nodiscard]] static bool less(const Def* a, const Def* b);
    [[nodiscard]] static bool greater(const Def* a, const Def* b);
    ///@}

    /// @name dot
    /// Streams dot to @p os, configured via @p cfg (see DotConfig).
    ///@{
    void dot(std::ostream& os, DotConfig cfg = {}) const;
    /// Same as above but write to @p file or `std::cout` if @p file is `nullptr`.
    void dot(const char* file = nullptr, DotConfig cfg = {}) const;
    void dot(const std::string& file, DotConfig cfg = {}) const { return dot(file.c_str(), cfg); }
    ///@}

protected:
    /// @name Wrappers for World::sym
    /// These are here to have Def::set%ters inline without including `mim/world.h`.
    ///@{
    Sym sym(const char*) const;
    Sym sym(std::string_view) const;
    Sym sym(std::string) const;
    ///@}

private:
    Defs reduce_(const Def* arg) const;
    virtual Def* stub_(World&, const Def*) { fe::unreachable(); }
    virtual const Def* rebuild_(World& w, const Def* type, Defs ops) const = 0;

    template<bool init>
    Vars free_vars(bool&, uint32_t);
    void invalidate();
    const Def** ops_ptr() const {
        return reinterpret_cast<const Def**>(reinterpret_cast<char*>(const_cast<Def*>(this + 1)));
    }
    bool equal(const Def* other) const;
    bool nests(Def*, MutSet&);

    template<Cmp>
    [[nodiscard]] static bool cmp_(const Def* a, const Def* b);

protected:
    mutable Dbg dbg_;
    union {
        NormalizeFn normalizer_; ///< Axm only: Axm%s use this member to store their normalizer.
        const Axm* axm_;         ///< App only: Curried App%s of Axm%s use this member to propagate the Axm.
        const Var* var_;         ///< Mutable only: Var of a mutable.
        Def* binder_;            ///< Var only: the binder this Var refers to (*not* an official op).
        mutable World* world_;
    };
    flags_t flags_;
    u8 curry_ = 0;
    u8 trip_  = 0;

private:
    Node node_; // 8
    bool mut_           : 1;
    bool external_      : 1;
    mutable bool annex_ : 1;
    bool dirty_         : 1;
    unsigned dep_       : 4;
    u32 mark_ = 0;
#ifndef NDEBUG
    size_t curr_op_ = 0;
#endif
    u32 gid_;
    u32 num_ops_;
    size_t hash_;
    Vars vars_; // Mutable: local vars; Immutable: free vars.
    Muts muts_; // Immutable: local_muts; Mutable: users;
    mutable u32 tid_ = 0;
    mutable const Def* type_;

    template<class D, size_t N>
    friend class Sets;
    friend class World;
    friend void swap(World&, World&) noexcept;
    friend std::ostream& operator<<(std::ostream&, const Def*);
};

/// A variable introduced by a binder (mutable).
/// @note Var will keep its type_ field as `nullptr`.
/// Instead, Def::type() and Var::type() will compute the type via Def::var_type().
/// The reason is that the type could need a Def::zonk().
/// But we don't want to have several Var%s that belong to the same binder.
class Var : public Def, public Setters<Var> {
private:
    Var(Def* mut)
        : Def(Node, mut) {}

public:
    using Setters<Var>::set;

    /// The binder of this Var.
    /// It is *not* an official Def::op but stored in Def::binder_, so it is out of the operand graph but still hashed.
    Def* binder() const { return binder_; }
    const Def* type() const { return binder()->var_type(); }

    static constexpr auto Node      = mim::Node::Var;
    static constexpr size_t Num_Ops = 0;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class Univ : public Def, public Setters<Univ> {
public:
    using Setters<Univ>::set;
    static constexpr auto Node      = mim::Node::Univ;
    static constexpr size_t Num_Ops = 0;

private:
    Univ(World& world)
        : Def(&world, Node, nullptr, Defs{}, 0) {}

    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class UMax : public Def, public Setters<UMax> {
public:
    using Setters<UMax>::set;
    static constexpr auto Node      = mim::Node::UMax;
    static constexpr size_t Num_Ops = std::dynamic_extent;

    enum Sort { Univ, Kind, Type, Term };

private:
    UMax(World&, Defs ops);

    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class UInc : public Def, public Setters<UInc> {
private:
    UInc(const Def* op, level_t offset)
        : Def(Node, op->type()->as<Univ>(), {op}, offset) {}

public:
    using Setters<UInc>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); }
    level_t offset() const { return flags(); }
    ///@}

    static constexpr auto Node      = mim::Node::UInc;
    static constexpr size_t Num_Ops = 1;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class Type : public Def, public Setters<Type> {
private:
    Type(const Def* level)
        : Def(Node, nullptr, {level}, 0) {}

public:
    using Setters<Type>::set;

    /// @name ops
    ///@{
    const Def* level() const { return op(0); }
    ///@}

    static constexpr auto Node      = mim::Node::Type;
    static constexpr size_t Num_Ops = 1;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class Lit : public Def, public Setters<Lit> {
private:
    Lit(const Def* type, flags_t val)
        : Def(Node, type, Defs{}, val) {}

public:
    using Setters<Lit>::set;

    /// @name Get actual Constant
    ///@{
    template<class T = flags_t>
    T get() const {
        static_assert(sizeof(T) <= 8);
        return bitcast_resize<T>(flags_);
    }
    ///@}

    using Def::as;
    using Def::isa;

    /// @name Casts
    ///@{
    /// @see @ref cast_lit
    template<class T = nat_t>
    static std::optional<T> isa(const Def* def) {
        if (!def) return {};
        if (auto lit = def->isa<Lit>()) return lit->get<T>();
        return {};
    }
    template<class T = nat_t>
    static T as(const Def* def) {
        return def->as<Lit>()->get<T>();
    }
    /// Like Lit::as but throws a formatted mim::error instead of merely asserting in `Debug`; see Def::expect.
    template<class T = nat_t, class... Args>
    static T expect(const Def* def, std::format_string<Args...> fmt, Args&&... args) {
        if (auto res = isa<T>(def)) return *res;
        fe::throwf("expected {}, but got '{}'", std::format(fmt, std::forward<Args>(args)...), def);
    }
    ///@}

    static constexpr auto Node      = mim::Node::Lit;
    static constexpr size_t Num_Ops = 0;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class Nat : public Def, public Setters<Nat> {
public:
    using Setters<Nat>::set;
    static constexpr auto Node      = mim::Node::Nat;
    static constexpr size_t Num_Ops = 0;

private:
    Nat(World& world);

    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

/// A built-in constant of type `Nat -> *`.
class Idx : public Def, public Setters<Idx> {
private:
    Idx(const Def* type)
        : Def(Node, type, Defs{}, 0) {}

public:
    using Setters<Idx>::set;
    using Def::as;
    using Def::isa;

    /// @name isa
    ///@{

    /// Checks if @p def is a `Idx s` and returns `s` or `nullptr` otherwise.
    static const Def* isa(const Def* def);
    static const Def* as(const Def* def) {
        auto res = isa(def);
        assert(res);
        return res;
    }
    static std::optional<nat_t> isa_lit(const Def* def);
    static nat_t as_lit(const Def* def) {
        auto res = isa_lit(def);
        assert(res.has_value());
        return *res;
    }
    ///@}

    /// @name Convert between Idx::isa and bitwidth and vice versa
    ///@{
    // clang-format off
    static constexpr nat_t bitwidth2size(nat_t n) { assert(n != 0); return n == 64 ? 0 : (1_n << n); }
    static constexpr nat_t size2bitwidth(nat_t n) { return n == 0 ? 64 : std::bit_width(n - 1_n); }
    // clang-format on
    static std::optional<nat_t> size2bitwidth(const Def* size);

    /// Yields the bit width of the `Idx` @p type or throws a formatted mim::error - instead of yielding
    /// std::nullopt or dereferencing an unchecked std::optional - if @p type is not an `Idx` of statically known
    /// size; see Def::expect.
    template<class... Args>
    static nat_t expect_bitwidth(const Def* type, std::format_string<Args...> fmt, Args&&... args) {
        if (auto size = isa(type))
            if (auto w = size2bitwidth(size)) return *w;
        fe::throwf("expected {}, but got '{}'", std::format(fmt, std::forward<Args>(args)...), type);
    }
    ///@}

    static constexpr auto Node      = mim::Node::Idx;
    static constexpr size_t Num_Ops = 0;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

class Proxy : public Def, public Setters<Proxy> {
private:
    Proxy(const Def* type, flags_t tag, Defs ops)
        : Def(Node, type, ops, tag) {}

public:
    using Setters<Proxy>::set;

    /// @name Getters
    ///@{
    flags_t tag() const { return flags_; }
    ///@}

    template<flags_t Tag>
    static const Proxy* isa(const Def* def) {
        if (auto proxy = def->isa<Proxy>(); proxy && proxy->tag() == Tag) return proxy;
        return nullptr;
    }

    static constexpr auto Node      = mim::Node::Proxy;
    static constexpr size_t Num_Ops = std::dynamic_extent;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

/// @deprecated A global variable in the data segment.
/// A Global may be mutable or immutable.
/// @deprecated Will be removed.
class Global : public Def, public Setters<Global> {
private:
    Global(const Def* type, bool is_mutable)
        : Def(Node, type, 1, is_mutable) {}

public:
    using Setters<Global>::set;

    /// @name ops
    ///@{
    const Def* init() const { return op(0); }
    void set(const Def* init) { Def::set(0, init); }
    ///@}

    /// @name type
    ///@{
    const App* type() const;
    const Def* alloced_type() const;
    ///@}

    /// @name Getters
    ///@{
    bool is_mutable() const { return flags(); }
    ///@}

    /// @name Rebuild
    ///@{
    Global* stub(const Def* type) { return stub_(world(), type)->set(dbg()); }
    ///@}

    static constexpr auto Node      = mim::Node::Global;
    static constexpr size_t Num_Ops = 1;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;
    Global* stub_(World&, const Def*) final;

    friend class World;
};

// Def - hot inline definitions
// These need Univ, Type, Var, and Lit to be complete, so they live here rather than in the class body.
// They are tiny and called millions of times, and `libmim` is a shared object - out of line they would be
// opaque PLT calls in every other TU.
inline World& Def::world() const noexcept {
    if (auto var = isa<Var>()) return var->binder()->world();

    for (auto def = this;; def = def->type()) {
        if (def->isa<Univ>()) return *def->world_;
        if (auto type = def->isa<Type>()) return *type->level()->type()->as<Univ>()->world_;
    }
}

inline const Def* Def::type() const noexcept {
    if (auto var = isa<Var>()) return var->binder()->var_type();
    return type_;
}

inline bool Def::equal(const Def* other) const {
    if (isa<Univ>() || this->isa_mut() || other->isa_mut()) return this == other;

    // A Var carries no ops and flags == 0, so it is identified solely by its binder (stored in binder_).
    if (auto var = isa<Var>()) return other->isa<Var>() && var->binder() == other->as<Var>()->binder();

    bool result = this->node() == other->node() && this->flags() == other->flags()
               && this->num_ops() == other->num_ops() && this->type() == other->type();

    for (size_t i = 0, e = num_ops(); result && i != e; ++i)
        result &= this->op(i) == other->op(i);

    return result;
}

inline nat_t Def::num_projs() const { return Lit::isa(arity()).value_or(1); }

} // namespace mim

#ifndef DOXYGEN // clang-format off
/// Format any pointer to a `mim::Def` (or subclass) via its `operator<<`.
template<class T> requires std::derived_from<T, mim::Def> struct std::formatter<      T*> : fe::ostream_formatter {};
template<class T> requires std::derived_from<T, mim::Def> struct std::formatter<const T*> : fe::ostream_formatter {};
template<> struct std::formatter<mim::Muts> : fe::ostream_formatter {};
template<> struct std::formatter<mim::Vars> : fe::ostream_formatter {};
#endif // clang-format on
