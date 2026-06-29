#include <mim/axm.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include "mim/plug/buffer/buffer.h"

namespace mim::plug::buffer {

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
