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
    if (auto ex = buf->isa<Extract>())
        if (auto cst = Axm::isa<constant>(ex->tuple())) {
            auto [cmem, v] = cst->arg()->projs<2>();
            return world.tuple({mem, v});
        }
    return {};
}

const Def* normalize_write(const Def*, const Def*, const Def*) { return {}; }

/// `shape buf i` ↦ the `i`-th size, read off the buffer's type.
const Def* normalize_shape(const Def* type, const Def*, const Def* arg) {
    auto& world       = type->world();
    auto [buf, index] = arg->projs<2>();
    auto [r, s, T]    = Axm::isa<Buf, false>(buf->type())->args<3>();
    return world.extract(s, index);
}

MIM_buffer_NORMALIZER_IMPL

} // namespace mim::plug::buffer
