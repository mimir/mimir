#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Rewrites the `core` and `mem` operations of a lowered world into x86 instructions.
/// Pointers become `I64`; everything else this phase does not know is an error.
class ISel : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Byte offset of `mem.lea (p, index)` into an old_world() @p pointee as an `I64` of new_world().
    const Def* offset(const Def* pointee, const Def* index);

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::x86::phase
