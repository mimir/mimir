#pragma once

#include <mim/phase.h>

namespace mim::plug::core::phase {

/// Widens each `Idx s` with a literal size `s` that is no `Bool`, `I8`, `I16`, `I32`, or `I64` to the next of these.
/// The values stay the same, so whatever only compares, shifts right, or divides them keeps working on the wider `Idx`.
/// Each core operation that depends on the modulus `s` (see isa_odd_idx_op) is computed in `I64` and reduced.
/// Where `Idx s` must stay, e.g. as the index of an `s`-tuple, the value is converted back; it is always in range.
class LegalizeIdx : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// The new_world() twin of the old_world() index @p type with each `Idx` kept at its original size.
    const Def* index_type(const Def* type);
    /// The old_world() @p index, rewritten but converted back to its original size.
    const Def* index(const Def* index);

    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_imm_Insert(const Insert*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;
};

} // namespace mim::plug::core::phase
