#pragma once

#include <mim/phase.h>

namespace mim::plug::core::phase {

/// Widens each `Idx s` with a literal size `s` that is no `Bool`, `I8`, `I16`, `I32`, or `I64` to the next of these.
/// The values stay the same, so whatever only compares, shifts right, or divides them keeps working on the wider `Idx`.
/// Each core operation that depends on the modulus `s` - core.wrap, core.conv and core.bitcast into `Idx s`, and every
/// signed interpretation - is computed in `I64` instead and explicitly reduced modulo `s`.
/// Where `Idx s` must stay, e.g. as the index of an `s`-tuple, the value is converted back; it is always in range.
class LegalizeIdx : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// @name I64 Arithmetic
    /// All arguments and results live in new_world().
    ///@{
    const Def* zext(const Def* x);           ///< The unsigned value of @p x as `I64`.
    const Def* sext(nat_t s, const Def* x);  ///< The signed representative of @p x in `Idx s` as `I64`.
    const Def* mod_u(nat_t s, const Def* y); ///< The unsigned `I64` @p y modulo @p s as a widened `Idx s`.
    const Def* mod_s(nat_t s, const Def* y); ///< The signed `I64` @p y modulo @p s as a widened `Idx s`.
    const Def* add(nat_t s, const Def* a, const Def* b);
    const Def* sub(nat_t s, const Def* a, const Def* b);
    const Def* mul(nat_t s, const Def* a, const Def* b);
    ///@}

    /// The new_world() twin of the old_world() index @p type with each `Idx` kept at its original size.
    const Def* index_type(const Def* type);

    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_imm_Insert(const Insert*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;
};

} // namespace mim::plug::core::phase
