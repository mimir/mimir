#include <mim/axm.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include "mim/plug/buffer/buffer.h"

namespace mim::plug::buffer {

/// `Buf (r, s, T)` with literal size-1 axes in `s` ↦ `Buf (r', s', T)` with those axes dropped.
/// This mirrors the folding of the corresponding array types (`«3, 1; T»` ≡ `«3; T»`), so buffer types
/// derived from logical shapes agree with the (folded) boundary types by construction.
const Def* normalize_Buf(const Def*, const Def* callee, const Def* arg) {
    auto& world    = arg->world();
    auto [r, s, T] = arg->projs<3>();
    auto r_l       = Lit::isa<u64>(r);
    if (!r_l) return {};

    DefVec dims;
    dims.reserve(*r_l);
    for (u64 i = 0; i < *r_l; ++i) {
        auto d = s->proj(*r_l, i);
        if (auto l = Lit::isa<u64>(d); l && *l == 1) continue;
        dims.push_back(d);
    }
    if (dims.size() == *r_l) return {};
    return world.app(callee, world.tuple({world.lit_nat(dims.size()), world.tuple(dims), T}));
}

/// `read (constant v) idx` ↦ `v`.
const Def* normalize_read(const Def* type, const Def*, const Def* arg) {
    auto& world            = type->world();
    auto [mem, buf, index] = arg->projs<3>();
    // FIXME unsound: ignores any buffer.write through this handle, including one in a callee.
    if (auto ex = buf->isa<Extract>())
        if (auto l = Axm::isa<lit>(ex->tuple())) {
            auto [cmem, v] = l->arg()->projs<2>();
            return world.tuple({mem, v});
        }
    return {};
}

const Def* normalize_write(const Def*, const Def*, const Def*) { return {}; }

/// `shape (r, s, T) (buf, i)` ↦ `s#i`; the logical shape, since the type of `buf` has its size-1 axes folded away.
const Def* normalize_shape(const Def* type, const Def* callee, const Def* arg) {
    auto index     = arg->proj(2, 1);
    auto [r, s, T] = callee->as<App>()->args<3>();
    return type->world().extract(s, index);
}

MIM_buffer_NORMALIZER_IMPL

} // namespace mim::plug::buffer
