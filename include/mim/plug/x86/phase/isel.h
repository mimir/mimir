#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Rewrites the `core` and `mem` operations of a lowered world into x86 instructions.
/// Pointers and `Nat` become `I64`; everything else this phase does not know is an error.
class ISel : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Zero-extends @p x of new_world() to `Idx d`, folding literals.
    const Def* zext(const Def* d, const Def* x);
    /// Byte offset of `mem.lea (p, index)` into an old_world() @p pointee, yielding a pointer to @p elem, as an `I64`
    /// of new_world().
    const Def* offset(const Def* pointee, const Def* elem, const Def* index);

    /// The rewritten @p def with a `Nat` literal as `I64`, for value operands.
    const Def* val(const Def* def);
    /// Applies @p insn, going through `i32` for operands narrower than 16 bits.
    const Def*
    wide(const Def* (*insn)(World&, const Def*, const Def*), const Def* type, bool sign, const Def*, const Def*);

    const Def* rewrite_imm_Nat(const Nat*) final { return new_world().type_i64(); }
    const Def* rewrite_imm_Lit(const Lit*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;
};

} // namespace mim::plug::x86::phase
