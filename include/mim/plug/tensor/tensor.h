#pragma once

#include <mim/world.h>

#include "mim/plug/tensor/autogen.h"

namespace mim::plug::tensor {

inline const Def* op_get(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index) {
    auto& w = arr->world();
    auto f  = w.annex<tensor::get>();
    f       = w.app(f, {T, r, s});
    f       = w.app(f, {arr, index});
    return f;
}

inline const Def* op_set(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index, const Def* x) {
    auto& w = arr->world();
    auto f  = w.app(w.annex<tensor::set>(), {T, r, s});
    f       = w.app(f, {arr, index, x});
    return f;
}

} // namespace mim::plug::tensor
