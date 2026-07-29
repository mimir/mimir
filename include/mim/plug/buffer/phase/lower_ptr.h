#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::buffer {

/// Lowers the `buffer` abstraction to the low-level pointer representation.
/// The `Buf` type is replaced by a pointer to nested arrays.
/// - `alloc` is replaced with `%mem.alloc`
/// - `read` becomes `%mem.lea` + `%mem.load`
/// - `write` becomes `%mem.lea` + `%mem.store`
class LowerPtr : public RWPhase {
public:
    LowerPtr(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) override;
};

} // namespace mim::plug::buffer
