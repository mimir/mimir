#pragma once

#include <mim/phase.h>

namespace mim::plug::mem::phase {

/// Rewrites every `mem.Ptr (T, a)` to the byte pointer `mem.Ptr (I8, a)` and lowers `mem.lea` to `mem.ptradd` with the
/// layout of `core.trait`, so that a native backend sees addresses as byte offsets.
/// An operation that needs a typed pointer gets a `core.bitcast` of the byte pointer.
class LegalizePtr : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;
};

} // namespace mim::plug::mem::phase
