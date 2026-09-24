#pragma once

#include <array>
#include <variant>

#include "mim/def.h"

namespace mim {

class Extract;

/// A [dependent function type](https://en.wikipedia.org/wiki/Dependent_type#%CE%A0_type).
/// @see Lam
class Pi : public Def, public Setters<Pi> {
protected:
    /// Constructor for an *immutable* Pi.
    Pi(const Def* type, const Def* dom, const Def* codom, bool implicit)
        : Def(Node, type, {dom, codom}, (flags_t)implicit) {}
    /// Constructor for a *mutable* Pi.
    Pi(const Def* type, bool implicit)
        : Def(Node, type, 2, implicit ? 1 : 0) {}

public:
    /// @name Get/Set implicit
    ///@{
    bool is_implicit() const { return flags(); }
    Pi* make_implicit() { return flags_ = (flags_t) true, this; }
    Pi* make_explicit() { return flags_ = (flags_t) false, this; }
    /// Is @p d an Pi::is_implicit (mutable) Pi?
    static Pi* isa_implicit(const Def* d) {
        if (auto pi = d->isa_mut<Pi>(); pi && pi->is_implicit()) return pi;
        return nullptr;
    }
    ///@}

    /// @name dom & codom
    /// @anchor pi_dom
    /// @see @ref proj
    ///@{
    const Def* dom() const { return op(0); }
    const Def* codom() const { return op(1); }
    MIM_PROJ(dom, const)
    MIM_PROJ(codom, const)
    ///@}

    /// @name Continuations
    /// @anchor continuations
    /// A *continuation* is a Pi whose Pi::codom is mim::Bot%tom.
    /// It is *returning*, if it has a Pi::ret_pi, and a *basic block* otherwise.
    ///@{
    // clang-format off
    static const Pi* isa_cn        (const Def* d) { auto pi = d->isa<Pi>(); return pi && pi->codom()->node() == Node::Bot ? pi : nullptr; }
    static const Pi* isa_returning (const Def* d) { auto pi = isa_cn(d); return pi &&  pi->ret_pi() ? pi : nullptr; }
    static const Pi* isa_basicblock(const Def* d) { auto pi = isa_cn(d); return pi && !pi->ret_pi() ? pi : nullptr; }
    // clang-format on

    /// Yields the last Pi::dom, if Pi::isa_basicblock.
    const Pi* ret_pi() const;
    /// Pi::dom%ain of Pi::ret_pi.
    const Def* ret_dom() const { return ret_pi()->dom(); }
    ///@}

    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Pi>::set;
    Pi* set(const Def* dom, const Def* codom) { return Def::set({dom, codom})->as<Pi>(); }
    Pi* set_dom(const Def* dom) { return Def::set(0, dom)->as<Pi>(); }
    Pi* set_dom(Defs doms);
    Pi* set_codom(const Def* codom) { return Def::set(1, codom)->as<Pi>(); }
    Pi* unset() { return Def::unset()->as<Pi>(); }
    ///@}

    /// @name Type Checking
    ///@{
    static const Def* infer(const Def* dom, const Def* codom);
    ///@}

    /// @name Reduce
    ///@{
    const Def* reduce(const Def* arg) const { return Def::reduce(arg).front(); }
    ///@}

    static constexpr auto Node      = mim::Node::Pi;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// A function.
/// @see Pi
class Lam : public Def, public Setters<Lam> {
private:
    Lam(const Pi* pi, const Def* filter, const Def* body)
        : Def(Node, pi, {filter, body}, 0) {}
    Lam(const Pi* pi)
        : Def(Node, pi, 2, 0) {}

public:
    using Filter = std::variant<bool, const Def*>;

    /// @name ops
    ///@{
    const Def* filter() const { return op(0); }
    const Def* body() const { return op(1); }
    ///@}

    /// @name type
    /// @anchor lam_dom
    /// @see @ref proj
    ///@{
    const Pi* type() const { return Def::type()->as<Pi>(); }
    const Def* dom() const { return type()->dom(); }
    const Def* codom() const { return type()->codom(); }
    /// Does this type() itself or dom() has a Var?
    bool is_dependent() const { return type()->has_var() || dom()->has_var(); }
    MIM_PROJ(dom, const)
    MIM_PROJ(codom, const)
    ///@}

    /// @name Continuations
    /// @see @ref continuations "Pi: Continuations"
    ///@{
    // clang-format off
    static const Lam* isa_cn            (const Def* d) { auto lam = d->isa    <Lam>(); return lam && Pi::isa_cn        (lam->type()) ? lam : nullptr; }
    static const Lam* isa_returning     (const Def* d) { auto lam = d->isa    <Lam>(); return lam && Pi::isa_returning (lam->type()) ? lam : nullptr; }
    static const Lam* isa_basicblock    (const Def* d) { auto lam = d->isa    <Lam>(); return lam && Pi::isa_basicblock(lam->type()) ? lam : nullptr; }
    static       Lam* isa_mut_cn        (const Def* d) { auto lam = d->isa_mut<Lam>(); return lam && Pi::isa_cn        (lam->type()) ? lam : nullptr; }
    static       Lam* isa_mut_returning (const Def* d) { auto lam = d->isa_mut<Lam>(); return lam && Pi::isa_returning (lam->type()) ? lam : nullptr; }
    static       Lam* isa_mut_basicblock(const Def* d) { auto lam = d->isa_mut<Lam>(); return lam && Pi::isa_basicblock(lam->type()) ? lam : nullptr; }
    // clang-format on

    const Pi* ret_pi() const { return type()->ret_pi(); }
    const Def* ret_dom() const { return ret_pi()->dom(); }
    /// Yields the Lam::var of the Lam::ret_pi.
    const Def* ret_var() {
        if (!ret_pi()) return nullptr;
        auto n = num_vars(); // compute the arity once and hand it to the (a, i) projection
        return var(n, n - 1);
    }
    /// Yields the `y` of `lm (x, ret) = ret y` - the argument @p d's body hands to its Lam::ret_var.
    /// `nullptr` if @p d is not a set, mutable Lam with such a body.
    static const Def* isa_ret_arg(const Def* d);
    ///@}

    /// @name Setters
    /// Lam::Filter is a `std::variant<bool, const Def*>` that lets you set the Lam::filter() like this:
    /// ```cpp
    /// lam1->app(true, f, arg);
    /// lam2->app(my_filter_def, f, arg);
    /// ```
    /// @note The filter belongs to the *Lam%bda* and **not** the body.
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Lam>::set;
    Lam* set(Filter filter, const Def* body);
    Lam* set_filter(Filter);                                                ///< Set filter first.
    Lam* set_body(const Def* body) { return Def::set(1, body)->as<Lam>(); } ///< Set body second.
    /// Set body to an App of @p callee and @p arg.
    Lam* app(Filter filter, const Def* callee, const Def* arg);
    /// Set body to an App of @p callee and @p args.
    Lam* app(Filter filter, const Def* callee, Defs args);
    /// Set body to an App of `(f, t)#cond mem` or `(f, t)#cond ()` if @p mem is `nullptr`.
    Lam* branch(Filter filter, const Def* cond, const Def* t, const Def* f, const Def* arg = nullptr);
    Lam* set(Defs ops) { return Def::set(ops)->as<Lam>(); }
    Lam* unset() { return Def::unset()->as<Lam>(); }
    ///@}

    /// @name Reduce
    ///@{
    using Def::reduce;
    Defs reduce(Defs) const;
    const Def* reduce_body(const Def* arg) const { return reduce(arg).back(); }
    ///@}

    /// @name Eta-Conversion
    ///@{
    static Lam* eta_expand(Filter, const Def* f);
    static Lam* eta_expand(const Def* f) { return eta_expand(true, f); } ///< Use `true` Filter.
    /// Yields the callee of body(), if eta-convertible and `nullptr` otherwise.
    /// η-convertible means: `lm x = f x` where `x` ∉ `f`.
    const Def* eta_reduce() const;
    ///@}

    /// @name Rewritable
    ///@{
    /// Is `this` neither is_external() nor is_annex() but is_set()?
    bool is_rewritable() { return !is_external() && !is_annex() && is_set(); }
    /// Yields @p def as a mutable Lam, if it is_rewritable() and `nullptr` otherwise.
    static Lam* isa_rewritable(const Def* def) {
        if (auto lam = def->isa_mut<Lam>(); lam && lam->is_rewritable()) return lam;
        return nullptr;
    }
    ///@}

    static constexpr auto Node      = mim::Node::Lam;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// @name Lam
/// GIDSet / GIDMap keyed by Lam::gid of `Lam*`.
///@{
template<class To>
using LamMap  = GIDMap<Lam*, To>;
using LamSet  = GIDSet<Lam*>;
using Lam2Lam = LamMap<Lam*>;
///@}

class App : public Def, public Setters<App> {
private:
    App(const Axm* axm, u8 curry, u8 trip, const Def* type, const Def* callee, const Def* arg)
        : Def(Node, type, {callee, arg}, 0) {
        axm_   = axm;
        curry_ = curry;
        trip_  = trip;
    }

    template<size_t N>
    static auto uncurry_args_(const Def* def) {
        auto args = std::array<const Def*, N>();
        for (size_t i = N; i-- != 0;)
            if (auto app = def->isa<App>()) args[i] = app->arg(), def = app->callee();
        return args;
    }

public:
    using Setters<App>::set;

    /// @name callee
    ///@{
    const Def* callee() const { return op(0); }
    const App* decurry() const { return callee()->as<App>(); } ///< Returns App::callee again as App.
    const Pi* callee_type() const { return callee()->type()->as<Pi>(); }
    /// The App::callee of @p def - or `nullptr`, if @p def isn't an App at all.
    static const Def* callee_of(const Def* def) {
        auto app = def->isa<App>();
        return app ? app->callee() : nullptr;
    }
    ///@}

    /// @name arg
    /// @anchor app_arg
    /// @see @ref proj
    ///@{
    const Def* arg() const { return op(1); }
    MIM_PROJ(arg, const)
    ///@}

    /// @name Get axm, current curry counter and trip count
    ///@{
    const Axm* axm() const { return axm_; }
    u8 curry() const { return curry_; }
    u8 trip() const { return trip_; }
    ///@}

    /// @name Uncurry
    /// Peel off the @p N innermost App%s of a curried App.
    /// Use like this:
    /// ```
    /// auto [abc, de]  = app->uncurry_args<2>();
    /// auto [a, b, c]  = abc->projs<3>();
    /// auto [d, e]     = de->projs<2>();
    /// auto axm        = app->uncurry_callee();
    /// ```
    /// If you "overshoot" the number of curried App%s, the superflous args on the left will be `nullptr`.
    ///@{
    // clang-format off
    template<size_t N> static auto uncurry_args(const Def* def) { return uncurry_args_<N>(def ); }
    template<size_t N>        auto uncurry_args() const         { return uncurry_args_<N>(this); }

    /// The innermost App::callee - the one that isn't an App itself.
    static const Def* uncurry_callee(const Def* def) { while (auto app = def->isa<App>()) def = app->callee(); return def; }
           const Def* uncurry_callee() const         { return uncurry_callee(this); }
    // clang-format on
    ///@}

    static constexpr auto Node      = mim::Node::App;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

} // namespace mim
