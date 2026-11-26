#pragma once

#include <exception>

#include "mim/phase.h"

namespace mim {

/// An RWPhase that lifts direct-style calls of CPS functions out of the bodies of continuations.
///
/// A subclass calls lift() for a call `k arg` of a CPS `k: Cn [T, Cn U]` while rewriting a continuation's body.
/// It allocates a fresh continuation `cont` receiving the result and records the pending call `k (arg, cont)`;
/// `cont`'s variable is the value of the call.
/// Once the enclosing Lam is done, the pending calls are wired up in encounter order - which respects
/// data dependencies, because operands are always rewritten before their users:
/// ```
/// lam   = k₀ (arg₀, cont₀)
/// cont₀ = k₁ (arg₁, cont₁)
/// ...
/// contₙ = rewritten body of lam
/// ```
/// A lifted call simply lives in the Lam whose body rewriting first reaches it.
/// Rewrites whose result mentions such a continuation variable are scoped to that Lam and re-derived - and thus
/// re-lifted - in sibling scopes.
class DS2CPS : public RWPhase {
public:
    using RWPhase::RWPhase;

protected:
    /// RAII guard for rewriting the body of one mutable Lam:
    /// fences pending_, pushes a map scope for contaminated rewrites, and tracks whether lifting is legal here.
    class Scope {
    public:
        Scope(DS2CPS& phase, bool liftable)
            : phase_(phase)
            , base_(phase.pending_.size())
            , old_liftable_(phase.liftable_) {
            phase_.push(Vars());
            phase_.liftable_ = liftable;
        }
        ~Scope() {
            assert((std::uncaught_exceptions() || phase_.pending_.size() == base_) && "pending calls must be wire()d");
            phase_.pop();
            phase_.liftable_ = old_liftable_;
        }

        size_t base() const { return base_; }

    private:
        DS2CPS& phase_;
        size_t base_;
        bool old_liftable_;
    };

    /// Is the body of a continuation being rewritten, so that lift() is legal?
    bool liftable() const { return liftable_; }
    /// Records the call `k (new_args..., cont)` as pending and returns `cont: Cn new_type`, whose Var is the result.
    Lam* lift(const Def* k, Defs new_args, const Def* new_type, bool filter = false);
    /// Wires all pending calls above @p base around @p body and returns the new body.
    const Def* wire(size_t base, const Def* body);

    std::pair<const Def*, const Def*> rewrite_ops(Lam*, Lam*) override;

private:
    /// A lifted call `callee (args..., cont)` waiting to be wired into the body of the enclosing Lam.
    struct Pending {
        const Def* callee;
        DefVec args;
        Lam* cont;
    };

    fe::Vector<Pending> pending_;
    bool liftable_ = false;
};

} // namespace mim
