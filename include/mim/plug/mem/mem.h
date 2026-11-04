#pragma once

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>

#include "mim/plug/mem/autogen.h"

namespace mim::plug::mem {

/// @name mem.M and mem.Ptr
/// mem.M and mem.Ptr are core.M and core.Ptr; their helpers live in mim::plug::core.
///@{
using core::has_leading_mem;
using core::has_mem_ty;
using core::isa_mem;
using core::mem_def;
using core::mem_var;
using core::mut_con;
using core::pointee;
using core::strip_mem_ty;
///@}

/// @name mem.lea
///@{
inline const Def* op_lea(const Def* ptr, const Def* index) {
    World& w                   = ptr->world();
    auto [pointee, addr_space] = Axm::as<Ptr>(ptr->type())->args<2>();
    auto Ts                    = tuple_of_types(pointee);
    return w.app(w.app(w.annex<lea>(), {pointee->arity(), Ts, addr_space}), {ptr, index});
}

/// Successively offsets @p ptr by each component of the index @p tuple, peeling one array dimension per `mem.lea`.
inline const Def* op_lea_tuple(const Def* ptr, const Def* tuple) {
    auto n       = tuple->num_projs();
    auto element = ptr;
    for (size_t i = 0; i != n; ++i)
        element = op_lea(element, tuple->proj(n, i));
    return element;
}
///@}

/// @name mem.alloc
///@{
inline const Def* op_alloc(const Def* type, const Def* as, const Def* mem) {
    World& w = type->world();
    return w.app(w.app(w.annex<alloc>(), {type, as}), mem);
}
inline const Def* op_alloc(const Def* type, const Def* mem) { return op_alloc(type, type->world().lit_nat_0(), mem); }
///@}

/// @name mem.slot
///@{
/// Builds the continuation-based `mem.slot (type, as)` applied to @p mem and the continuation @p ret.
/// @p ret is a `Cn [mem.M as, mem.Ptr (type, as)]` receiving the fresh slot; its scope delimits the slot's lifetime.
inline const Def* op_slot(const Def* type, const Def* as, const Def* mem, const Def* ret) {
    World& w = type->world();
    return w.app(w.app(w.annex<slot>(), {type, as}), {mem, ret});
}
/// Decomposes the continuation-based @p slot into `(mem, ret_lam, ret_mem, ptr)`,
/// where `ret_mem` is the continuation's mem and `ptr` is the continuation's slot var;
/// the slot's lifetime is `ret_lam`'s scope.
/// All but `mem` are `nullptr` if the continuation is no mutable Lam.
inline std::tuple<const Def*, Lam*, const Def*, const Def*> split_slot(const App* slot) {
    auto [mem, ret] = slot->args<2>();
    if (auto ret_lam = ret->isa_mut<Lam>()) return {mem, ret_lam, ret_lam->var(2, 0), ret_lam->var(2, 1)};
    return {mem, nullptr, nullptr, nullptr};
}

///@}

/// @name mem.malloc
///@{
inline const Def* op_malloc(const Def* type, const Def* as, const Def* mem) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<malloc>(), {type, as}), {mem, size});
}
///@}

/// @name mem.mslot
///@{
/// Builds the continuation-based `mem.mslot (type, as)` applied to `(mem, size)` and the continuation @p ret.
/// @p ret is a `Cn [mem.M as, mem.Ptr (type, as)]` receiving the fresh slot; its scope delimits the slot's lifetime.
inline const Def* op_mslot(const Def* type, const Def* as, const Def* mem, const Def* ret) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<mslot>(), {type, as}), {w.tuple({mem, size}), ret});
}
///@}

} // namespace mim::plug::mem
