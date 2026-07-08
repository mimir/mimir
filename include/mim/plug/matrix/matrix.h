#pragma once

#include <mim/world.h>

#include "mim/plug/buffer/buffer.h"
#include "mim/plug/matrix/autogen.h"

namespace mim::plug::matrix {

/// Reads element `idx` from a matrix (represented by a `%buffer.Buf`), threading `mem`.
inline const Def* op_read(const Def* mem, const Def* matrix, const Def* idx) {
    auto buf_ty = Axm::isa<buffer::Buf>(matrix->type());
    if (!buf_ty) return matrix;
    auto [n, S, T] = buf_ty->args<3>();
    matrix->world().DLOG("matrix read: {}[{}]", matrix, idx);
    return buffer::op_read(n, S, T, mem, matrix, idx);
}

} // namespace mim::plug::matrix
