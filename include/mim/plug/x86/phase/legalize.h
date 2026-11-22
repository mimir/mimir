#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Turns every `Nat` value and every pointer into an `I64`, so that instruction selection only sees register types.
/// `Nat` literals stay `Nat` as type-level arguments and become `I64` only as values.
/// `mem.ptradd` becomes `core.wrap.add`, the `Nat` operations become their `I64` counterparts, and `mem.mslot` becomes
/// `x86.slot`.
/// An operation that needs a typed pointer gets a `core.bitcast` of the `I64`.
class Legalize : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Are the rewritten @p args no type-level `Nat`s, such as a size bound by a Pi?
    bool is_value(const Def* args);
    /// The rewritten @p def with a `Nat` literal as `I64`.
    const Def* val(const Def* def);

    const Def* rewrite_imm_Nat(const Nat*) final { return new_world().type_i64(); }
    const Def* rewrite_imm_Lit(const Lit*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;
};

} // namespace mim::plug::x86::phase
