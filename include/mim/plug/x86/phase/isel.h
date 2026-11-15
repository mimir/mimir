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
    /// `base + index * scale + disp` in new_world(); a missing `index` is `nullptr`.
    struct Address {
        const Def* base;
        const Def* index = nullptr;
        nat_t scale      = 1;
        nat_t disp       = 0;
    };

    /// Folds the `mem.lea` chain of the old_world() pointer @p ptr.
    Address address(const Def* ptr);
    /// The `x86.addr` of @p a.
    const Def* addr(Address a);
    /// @p a as a value.
    const Def* materialize(Address a);

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
