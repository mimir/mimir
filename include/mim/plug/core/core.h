#pragma once

#include <algorithm>
#include <bit>

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/core/autogen.h"

namespace mim::plug::core {

constexpr flags_t icmp_mask = 0b00011111; // low 5 bits encode X Y G L E as bits 4..0

/// @name Mode
///@{
/// What should happen if Idx arithmetic overflows?
enum class Mode : nat_t {
    none = 0,      ///< Wrap around.
    nsw  = 1 << 0, ///< No Signed Wrap around.
    nuw  = 1 << 1, ///< No Unsigned Wrap around.
    nsuw = nsw | nuw,
};

/// Give Mode as mim::plug::core::Mode, mim::nat_t or const Def*.
using VMode = std::variant<Mode, nat_t, const Def*>;

/// mim::plug::core::VMode -> const Def*.
inline const Def* mode(World& w, VMode m) {
    if (auto def = std::get_if<const Def*>(&m)) return *def;
    if (auto nat = std::get_if<nat_t>(&m)) return w.lit_nat(*nat);
    return w.lit_nat(std::to_underlying(std::get<Mode>(m)));
}
///@}

/// The size of the smallest of `I8`, `I16`, `I32`, and `I64` that holds the values of `Idx s`.
inline nat_t widen_idx(nat_t s) { return Idx::bitwidth2size(std::max(8_n, std::bit_ceil(Idx::size2bitwidth(s)))); }

/// The literal size of the `Idx` @p type if it is no `Bool`, `I8`, `I16`, `I32`, or `I64`.
inline std::optional<nat_t> isa_odd_idx(const Def* type) {
    if (auto s = Idx::isa_lit(type); s && *s > 2 && widen_idx(*s) != *s) return s;
    return {};
}

inline const Def* zext(World& w, const Def* x) {
    return w.call(conv::u, w.lit_nat_0(), x);
} ///< Zero-extends @p x to `I64`.

/// Does core.icmp @p id give the same result on the canonical values of any `Idx s`?
inline bool is_unsigned(icmp id) {
    switch (id) {
        case icmp::f:
        case icmp::e:
        case icmp::ug:
        case icmp::uge:
        case icmp::ul:
        case icmp::ule:
        case icmp::ne:
        case icmp::t: return true;
        default: return false;
    }
}

/// The isa_odd_idx size whose modulus the core operation @p def depends on.
/// Neither core.wrap without unsigned wrap-around nor core.idx with a mode depends on it.
/// core.conv.u into such an `Idx` does, but core.legalize_idx emits that itself for values in range.
inline std::optional<nat_t> isa_odd_idx_op(const Def* def) {
    auto mode = [](const App* op) { return Lit::isa(op->decurry()->decurry()->arg()); };
    if (auto op = Axm::isa<wrap>(def)) {
        if (auto m = mode(op); m && (*m & nat_t(Mode::nuw))) return {};
        return isa_odd_idx(def->type());
    }
    if (auto op = Axm::isa<idx>(def)) return mode(op) == 0 ? isa_odd_idx(def->type()) : std::nullopt;
    if (Axm::isa<bitcast>(def)) return isa_odd_idx(def->type());
    if (auto op = Axm::isa<conv>(def); op && op.id() == conv::s)
        return isa_odd_idx(def->type()).or_else([&] { return isa_odd_idx(op->arg()->type()); });
    if (auto op = Axm::isa<icmp>(def); op && !is_unsigned(op.id())) return isa_odd_idx(op->arg(2, 0)->type());
    if (auto op = Axm::isa<shr>(def); op && op.id() == shr::a) return isa_odd_idx(def->type());
    if (auto op = Axm::isa<extrema>(def); op && (op.id() == extrema::smin || op.id() == extrema::smax))
        return isa_odd_idx(def->type());
    if (auto op = Axm::isa<abs>(def)) return isa_odd_idx(op->arg(2, 1)->type());
    if (auto op = Axm::isa<div>(def); op && (op.id() == div::sdiv || op.id() == div::srem))
        return isa_odd_idx(op->arg(2, 1)->proj(2, 0)->type());
    return {};
}

/// @name core.trait
///@{
inline const Def* op(trait o, const Def* type) {
    World& w = type->world();
    return w.app(w.annex(o), type);
}
///@}

/// @name core.pe
///@{
inline const Def* op(pe o, const Def* def) {
    World& w = def->world();
    return w.app(w.app(w.annex(o), def->type()), def);
}
///@}

/// @name core.bit2
///@{
/// Use like this: `a op b = tab[a][b]`
constexpr std::array<std::array<u64, 2>, 2> make_truth_table(bit2 id) {
    return {
        {{sub_t(id) & sub_t(0b0001) ? u64(-1) : 0, sub_t(id) & sub_t(0b0010) ? u64(-1) : 0},
         {sub_t(id) & sub_t(0b0100) ? u64(-1) : 0, sub_t(id) & sub_t(0b1000) ? u64(-1) : 0}}
    };
}
///@}

/// @name core.M
///@{

inline Lam* mut_con(World& w, nat_t a = 0) { return w.mut_con(w.call<M>(a)); } ///< Yields `con[core.M 0]`.

/// Yields `con[core.M 0, dom]`.
inline Lam* mut_con(const Def* dom) {
    World& w = dom->world();
    return w.mut_con({w.call<M>(0), dom});
}

/// If @p def is a `core.M`-typed value, yields its memory type `core.M a`; otherwise `nullptr`.
inline const App* isa_mem(const Def* def) { return Axm::isa<M>(def->type()); }

/// Does @p pi already thread memory - a leading `core.M`, either directly or grouped as the first
/// component of the first parameter (e.g. the `Fn [mem.M 0, To, ins] → …` shape of a mem-threaded combiner)?
inline bool has_leading_mem(const Pi* pi) {
    if (pi->num_doms() == 0) return false;
    auto dom0 = pi->dom(pi->num_doms(), 0);
    if (Axm::isa<M>(dom0)) return true;
    if (auto sig = dom0->isa<Sigma>(); sig && sig->num_ops() != 0 && Axm::isa<M>(sig->op(0))) return true;
    return false;
}

/// Returns the (first) element of type `core.M a` from the given tuple.
inline const Def* mem_def(const Def* def) {
    if (isa_mem(def)) return def;
    if (def->isa<Arr>() || def->type()->isa<Arr>()) return {}; // don't look into possibly gigantic arrays

    if (def->num_projs() > 1) {
        for (auto proj : def->projs())
            if (auto mem = mem_def(proj)) return mem;
    }

    return {};
}

/// Returns the memory argument of a function if it has one.
inline const Def* mem_var(Lam* lam) { return mem_def(lam->var()); }

/// Is @p type `core.M` or a Sigma with a `core.M` component?
inline bool has_mem_ty(const Def* type) {
    if (Axm::isa<M>(type)) return true;
    if (auto sigma = type->isa<Sigma>())
        return std::ranges::any_of(sigma->ops(), [](const Def* op) { return Axm::isa<M>(op); });
    return false;
}

/// Removes recusively all occurences of mem from a type (sigma).
inline const Def* strip_mem_ty(const Def* def) {
    auto& world = def->world();

    if (auto sigma = def->isa<Sigma>()) {
        DefVec new_ops;
        for (auto op : sigma->ops())
            if (auto new_op = strip_mem_ty(op); new_op != world.sigma()) new_ops.push_back(new_op);

        return world.sigma(new_ops);
    } else if (Axm::isa<M>(def)) {
        return world.sigma();
    }

    return def;
}
///@}

/// @name core.Ptr
///@{
inline const Def* pointee(const Def* ptr) {
    auto [pointee, _] = Axm::as<Ptr>(ptr->type())->args<2>();
    return pointee;
}
///@}

} // namespace mim::plug::core

namespace mim {

/// @name is_commutative/is_associative
///@{
// clang-format off
constexpr bool is_commutative(plug::core::nat  id) { return id == plug::core::nat ::add || id == plug::core::nat ::mul; }
constexpr bool is_commutative(plug::core::ncmp id) { return id == plug::core::ncmp::  e || id == plug::core::ncmp:: ne; }
constexpr bool is_commutative(plug::core::wrap id) { return id == plug::core::wrap::add || id == plug::core::wrap::mul; }
constexpr bool is_commutative(plug::core::icmp id) { return id == plug::core::icmp::  e || id == plug::core::icmp:: ne; }
// clang-format on

constexpr bool is_commutative(plug::core::bit2 id) {
    auto tab = make_truth_table(id);
    return tab[0][1] == tab[1][0];
}

constexpr bool is_associative(plug::core::bit2 id) {
    switch (id) {
        case plug::core::bit2::t:
        case plug::core::bit2::xor_:
        case plug::core::bit2::and_:
        case plug::core::bit2::nxor:
        case plug::core::bit2::fst:
        case plug::core::bit2::snd:
        case plug::core::bit2::or_:
        case plug::core::bit2::f: return true;
        default: return false;
    }
}

// clang-format off
constexpr bool is_associative(plug::core::nat  id) { return is_commutative(id); }
constexpr bool is_associative(plug::core::ncmp id) { return is_commutative(id); }
constexpr bool is_associative(plug::core::icmp id) { return is_commutative(id); }
constexpr bool is_associative(plug::core::wrap id) { return is_commutative(id); }
// clang-format on
///@}

} // namespace mim

#ifndef DOXYGEN
template<>
struct fe::is_bit_enum<mim::plug::core::Mode> : std::true_type {};
#endif
