#pragma once

#include <mim/axm.h>
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

/// Give Mode as mim::plug::math::Mode, mim::nat_t or const Def*.
using VMode = std::variant<Mode, nat_t, const Def*>;

/// mim::plug::core::VMode -> const Def*.
inline const Def* mode(World& w, VMode m) {
    if (auto def = std::get_if<const Def*>(&m)) return *def;
    if (auto nat = std::get_if<nat_t>(&m)) return w.lit_nat(*nat);
    return w.lit_nat(std::to_underlying(std::get<Mode>(m)));
}
///@}

/// @name %%core.trait
///@{
inline const Def* op(trait o, const Def* type) {
    World& w = type->world();
    return w.app(w.annex(o), type);
}
///@}

/// @name %%core.pe
///@{
inline const Def* op(pe o, const Def* def) {
    World& w = def->world();
    return w.app(w.app(w.annex(o), def->type()), def);
}
///@}

/// @name %%core.bit2
///@{
/// Use like this: `a op b = tab[a][b]`
constexpr std::array<std::array<u64, 2>, 2> make_truth_table(bit2 id) {
    return {
        {{sub_t(id) & sub_t(0b0001) ? u64(-1) : 0, sub_t(id) & sub_t(0b0100) ? u64(-1) : 0},
         {sub_t(id) & sub_t(0b0010) ? u64(-1) : 0, sub_t(id) & sub_t(0b1000) ? u64(-1) : 0}}
    };
}
///@}

/// @name extract_unsafe
///@{
inline const Def* extract_unsafe(const Def* d, const Def* i) {
    World& w = d->world();
    return w.extract(d, w.call(conv::u, d->unfold_type()->arity(), i));
}
inline const Def* extract_unsafe(const Def* d, u64 i) {
    World& w = d->world();
    return extract_unsafe(d, w.lit_idx(0_u64, i));
}
///@}

/// @name insert_unsafe
///@{
inline const Def* insert_unsafe(const Def* d, const Def* i, const Def* val) {
    World& w = d->world();
    return w.insert(d, w.call(conv::u, d->unfold_type()->arity(), i), val);
}
inline const Def* insert_unsafe(const Def* d, u64 i, const Def* val) {
    World& w = d->world();
    return insert_unsafe(d, w.lit_idx(0_u64, i), val);
}
///@}

/// @name Convert TBound to Sigma
/// This is WIP.
///@{
template<bool up>
const Sigma* convert(const TBound<up>* b);
inline const Sigma* convert(const Bound* b) { return b->isa<Join>() ? convert(b->as<Join>()) : convert(b->as<Meet>()); }
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
// clang-format off

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
