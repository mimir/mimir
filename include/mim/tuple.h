#pragma once

#include <span>

#include "mim/def.h"

namespace mim {

/// Base class for Sigma and Tuple.
class Prod : public Def, public Setters<Prod> {
protected:
    using Def::Def;

public:
    /// Prod groups Sigma and Tuple; see fe::NodeSetable.
    static constexpr bool isa_node(mim::Node n) noexcept { return n == mim::Node::Sigma || n == mim::Node::Tuple; }

    static constexpr size_t Num_Ops = std::dynamic_extent;
};

/// A [dependent tuple type](https://en.wikipedia.org/wiki/Dependent_type#%CE%A3_type).
/// @see Tuple, Arr, Pack, Prod
class Sigma : public Prod, public Setters<Sigma> {
private:
    Sigma(const Def* type, Defs ops)
        : Prod(Node, type, ops, 0) {} ///< Constructor for an *immutable* Sigma.
    Sigma(const Def* type, size_t size)
        : Prod(Node, type, size, 0) {} ///< Constructor for a *mutable* Sigma.

public:
    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Sigma>::set;
    Sigma* set(size_t i, const Def* def) { return Def::set(i, def)->as<Sigma>(); }
    Sigma* set(Defs ops) { return Def::set(ops)->as<Sigma>(); }
    Sigma* unset() { return Def::unset()->as<Sigma>(); }
    ///@}

    /// @name Rebuild
    ///@{
    /// @note Technically, it would make sense to have an offset of 1 as the first element can't be reduced.
    /// For example, in `[n: Nat, F n]` `n` only occurs free in the second element.
    /// However, this would cause a lot of confusion and special code to cope with the first element,
    /// So we just keep it.
    ///@}

    /// @name Type Checking
    ///@{
    static const Def* infer(World&, Defs);
    ///@}

    static constexpr auto Node = mim::Node::Sigma;

private:
    friend class World;
};

/// Data constructor for a Sigma.
/// @see Sigma, Arr, Pack, Prod
class Tuple : public Prod, public Setters<Tuple> {
public:
    using Setters<Tuple>::set;
    static const Def* infer(World&, Defs);
    static constexpr auto Node = mim::Node::Tuple;

private:
    Tuple(const Def* type, Defs args)
        : Prod(Node, type, args, 0) {}

    friend class World;
};

/// Base class for Arr and Pack.
class Seq : public Def, public Setters<Seq> {
protected:
    using Def::Def;

public:
    /// Seq groups Arr and Pack; see fe::NodeSetable.
    static constexpr bool isa_node(mim::Node n) noexcept { return n == mim::Node::Arr || n == mim::Node::Pack; }

    /// @name ops
    ///@{
    const Def* body() const { return ops().back(); }

    /// The extents of all axes this Seq fuses: a `Nat` for a one-dimensional Seq, an aggregate of `Nat`s otherwise.
    /// Def::arity is the *first* extent; `«(2, 3); T»` still projects into two `«3; T»`.
    const Def* shape() const { return op(0); }
    /// The element one axis down: Seq::body for an unfused Seq, the Seq of the remaining axes otherwise.
    const Def* elem() const;
    const Def* rank() const { return shape()->arity(); } ///< Number of fused axes; `1` for a one-dimensional Seq.
    /// Whether this Seq fuses more than one axis; `false` also for a *dynamic* rank that may yet turn out to be 1.
    bool is_fused() const {
        auto r = Lit::isa(rank());
        return !r || *r != 1;
    }
    ///@}

    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Seq>::set;

    /// Common setter for Pack%s and Arr%ays.
    Seq* set(const Def* shape, const Def* body) { return Def::set({shape, body})->as<Seq>(); }
    Seq* unset() { return Def::unset()->as<Seq>(); }
    ///@}

    /// @name Rebuild
    ///@{
    const Def* reduce(const Def* arg) const { return Def::reduce(arg).front(); }
    ///@}
};

/// A (possibly paramterized) Arr%ay.
/// Arr%ays are usually homogenous but they can be *inhomogenous* as well: `«i: N; T#i»`
/// @see Sigma, Tuple, Pack
class Arr : public Seq, public Setters<Arr> {
private:
    Arr(const Def* type, const Def* arity, const Def* body)
        : Seq(Node, type, {arity, body}, 0) {} ///< Constructor for an *immutable* Arr.
    Arr(const Def* type)
        : Seq(Node, type, 2, 0) {} ///< Constructor for a *mutable* Arr.

public:
    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Arr>::set;
    Arr* set_shape(const Def* shape) { return Def::set(0, shape)->as<Arr>(); }
    Arr* set_body(const Def* body) { return Def::set(1, body)->as<Arr>(); }
    Arr* set(const Def* shape, const Def* body) { return set_shape(shape)->set_body(body); }
    Arr* unset() { return Def::unset()->as<Arr>(); }
    ///@}

    static constexpr auto Node      = mim::Node::Arr;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// A (possibly paramterized) Tuple.
/// @see Sigma, Tuple, Arr
class Pack : public Seq, public Setters<Pack> {
private:
    Pack(const Def* type, const Def* shape, const Def* body)
        : Seq(Node, type, {shape, body}, 0) {} ///< Constructor for an *immutable* Pack.
    Pack(const Def* type)
        : Seq(Node, type, 2, 0) {} ///< Constructor for a *mutable* Pack.

public:
    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Pack>::set;
    /// @note A Pack carries its own Seq::shape: its type fuses *every* axis down to a non-array element,
    /// which is more than the Pack itself supplies whenever its body is an aggregate - `‹2; v›` for `v: «3; T»`.
    Pack* set_shape(const Def* shape) { return Def::set(0, shape)->as<Pack>(); }
    Pack* set_body(const Def* body) { return Def::set(1, body)->as<Pack>(); }
    Pack* set(const Def* shape, const Def* body) { return set_shape(shape)->set_body(body); }
    Pack* unset() { return Def::unset()->as<Pack>(); }
    ///@}

    static constexpr auto Node      = mim::Node::Pack;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// Extracts from a Sigma or Arr%ay-typed Extract::tuple the element at position Extract::index.
class Extract : public Def, public Setters<Extract> {
private:
    Extract(const Def* type, const Def* tuple, const Def* index)
        : Def(Node, type, {tuple, index}, 0) {}

public:
    using Setters<Extract>::set;

    /// @name ops
    ///@{
    const Def* tuple() const { return op(0); }
    const Def* index() const { return op(1); }
    ///@}

    static constexpr auto Node      = mim::Node::Extract;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// Creates a new Tuple / Pack by inserting Insert::value at position Insert::index into Insert::tuple.
/// @attention This is a *functional* Insert.
///     The Insert::tuple itself remains untouched.
///     The Insert itself is a *new* Tuple / Pack which contains the inserted Insert::value.
class Insert : public Def, public Setters<Insert> {
private:
    Insert(const Def* tuple, const Def* index, const Def* value)
        : Def(Node, tuple->type(), {tuple, index, value}, 0) {}

public:
    using Setters<Insert>::set;

    /// @name ops
    ///@{
    const Def* tuple() const { return op(0); }
    const Def* index() const { return op(1); }
    const Def* value() const { return op(2); }
    ///@}

    static constexpr auto Node      = mim::Node::Insert;
    static constexpr size_t Num_Ops = 3;

private:
    friend class World;
};

/// Matches `(ff, tt)#cond` - where `cond` is **not** a Lit%eral.
/// @note If `cond` is a Lit%eral, either
/// * `(x, y)#lit` would have been folded to `x`/`y` anyway, or
/// * we have something like this: `pair#0_2`
class Select {
public:
    Select(const Def*);

    explicit operator bool() const noexcept { return extract_; }

    const Extract* extract() const { return extract_; }
    const Def* pair() const { return extract()->tuple(); }
    const Def* cond() const { return extract()->index(); }
    const Def* tt() const { return pair()->proj(2, 1); }
    const Def* ff() const { return pair()->proj(2, 0); }

private:
    const Extract* extract_ = nullptr;
};

/// Matches `(ff, tt)#cond arg` where `cond` is **not** a Lit%eral.
/// `(ff, tt)#cond` is matched as a Select.
class Branch : public Select {
public:
    Branch(const Def*);

    explicit operator bool() const noexcept { return app_; }

    const App* app() const { return app_; }
    const Def* callee() const;
    const Def* arg() const;

private:
    const App* app_ = nullptr;
};

/// Matches a dispatch through a jump table of the form:
/// `(target_0, target_1, ...)#index arg` where `index` is **not** a Lit%eral.
/// @note Subsumes Branch.
/// If you want to deal with Branch separately, match Branch first:
/// ```
/// if (auto branch = Branch(def)) {
///     // special case first
/// } else if (auto dispatch = Dispatch(def)) {
///     // now, the generic case
/// }
/// ```
class Dispatch {
public:
    Dispatch(const Def*);

    explicit operator bool() const noexcept { return app_; }

    const App* app() const { return app_; }
    const Def* callee() const;
    const Def* arg() const;

    const Extract* extract() const { return extract_; }
    const Def* tuple() const { return extract()->tuple(); }
    const Def* index() const { return extract()->index(); }

    size_t num_targets() const { return Lit::as(extract()->tuple()->arity()); }
    const Def* target(size_t i) const { return tuple()->proj(i); }

private:
    const App* app_         = nullptr;
    const Extract* extract_ = nullptr;
};

/// @name Helpers to work with Tuples/Sigmas/Arrays/Packs
///@{
bool is_unit(const Def*);
std::string tuple2str(const Def*);

const Def* tuple_of_types(const Def* t);
///@}

/// @name Shapes
/// A *shape* is the extent of one axis (a `Nat`) or an aggregate of such extents, one per fused axis.
///@{
bool is_dim(const Def* shape);             ///< Does @p shape describe a single axis, i.e. is it a plain `Nat`?
const Def* first_extent(const Def* shape); ///< `shape#0` - the extent of the outermost axis.
/// Drops every literal-`1` axis, mirroring `«1; T»` ≡ `T`; a rank the Seq folds away entirely yields `()`.
/// Returns @p shape unchanged if its rank isn't a Lit.
const Def* fold_shape(const Def* shape);
/// Drops every `Idx 1` component of a multi-dimensional @p index, so that it lines up with fold_shape.
const Def* fold_index(const Def* index);
/// As above but driven by @p shape: drops the components of its literal size-1 axes, whatever the component's
/// own `Idx` size is - a broadcast reads a size-1 input axis at the *output*'s loop index.
const Def* fold_index(const Def* shape, const Def* index);
///@}

/// @name Concatenation
/// Works for Tuple%s, Pack%s, Sigma%s, and Arr%ays alike.
///@{
DefVec cat(Defs, Defs);
inline DefVec cat(const Def* a, Defs bs) { return cat(Defs{a}, bs); }
inline DefVec cat(Defs as, const Def* b) { return cat(as, Defs{b}); }

DefVec cat(nat_t n, nat_t m, const Def* a, const Def* b);

/// @p d's projections `[begin, end)` of @p r as a Tuple; the counterpart of cat_tuple.
const Def* slice_tuple(const Def* d, nat_t r, nat_t begin, nat_t end);

const Def* cat_tuple(nat_t n, nat_t m, const Def* a, const Def* b);
const Def* cat_sigma(nat_t n, nat_t m, const Def* a, const Def* b);

const Def* cat_tuple(World&, Defs, Defs);
const Def* cat_sigma(World&, Defs, Defs);

inline const Def* cat_tuple(const Def* a, Defs bs) { return cat_tuple(a->world(), Defs{a}, bs); }
inline const Def* cat_tuple(Defs as, const Def* b) { return cat_tuple(b->world(), as, Defs{b}); }
inline const Def* cat_sigma(const Def* a, Defs bs) { return cat_sigma(a->world(), Defs{a}, bs); }
inline const Def* cat_sigma(Defs as, const Def* b) { return cat_sigma(b->world(), as, Defs{b}); }
///@}

} // namespace mim
