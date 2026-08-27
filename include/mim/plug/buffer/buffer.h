#pragma once

#include <mim/world.h>

#include "mim/plug/buffer/autogen.h"

namespace mim::plug::buffer {

/// The buffer type `%buffer.Buf (r, s, T)`.
inline const Def* type_buf(const Def* r, const Def* s, const Def* T) {
    auto& w = r->world();
    return w.app(w.annex<Buf>(), {r, s, T});
}

/// `%buffer.alloc (r, s, T) mem` ↦ `[%mem.M 0, %buffer.Buf (r, s, T)]`.
inline const Def* op_alloc(const Def* r, const Def* s, const Def* T, const Def* mem) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<alloc>(), {r, s, T}), mem);
}

/// `%buffer.read (r, s, T) (mem, buf, idx)` ↦ `[%mem.M 0, T]`.
inline const Def* op_read(const Def* r, const Def* s, const Def* T, const Def* mem, const Def* buf, const Def* idx) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<read>(), {r, s, T}), {mem, buf, idx});
}

/// `%buffer.write (r, s, T) (mem, buf, idx, val)` ↦ `[%mem.M 0, %buffer.Buf (r, s, T)]`.
inline const Def*
op_write(const Def* r, const Def* s, const Def* T, const Def* mem, const Def* buf, const Def* idx, const Def* val) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<write>(), {r, s, T}), {mem, buf, idx, val});
}

/// `%buffer.copy (r, s, T) (mem, dst, src)` ↦ `%mem.M 0` (copies the whole buffer `src` into `dst`).
inline const Def* op_copy(const Def* r, const Def* s, const Def* T, const Def* mem, const Def* dst, const Def* src) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<copy>(), {r, s, T}), {mem, dst, src});
}

/// `%buffer.init (r, s, T) (mem, val)` ↦ `[%mem.M 0, %buffer.Buf (r, s, T)]` (initialised with the array value `val`).
inline const Def* op_init(const Def* r, const Def* s, const Def* T, const Def* mem, const Def* val) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<init>(), {r, s, T}), {mem, val});
}

/// `%buffer.lit (r, s, T) (mem, val)` ↦ `[%mem.M 0, %buffer.Buf (r, s, T)]` (every element initialised to `val`).
inline const Def* op_lit(const Def* r, const Def* s, const Def* T, const Def* mem, const Def* val) {
    auto& w = mem->world();
    return w.app(w.app(w.annex<lit>(), {r, s, T}), {mem, val});
}

} // namespace mim::plug::buffer
