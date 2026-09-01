#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::cps {

/// Full CPS conversion in one recursive rewrite.
///
/// Each term-level, direct-style function
/// ```
/// f: [a: A] → B
/// ```
/// becomes
/// ```
/// f_cps: Cn [a: A, Cn B]
/// ```
/// and every use of `f` is replaced by `%cps.cps2ds_dep (A, λ a: B) f_cps` to remain type-correct.
///
/// Call sites are lifted on the fly:
/// When the recursive rewrite encounters `App (%cps.cps2ds_dep (T, U) k) arg` inside a continuation,
/// it allocates a fresh continuation `cont` receiving the result, records the pending call `k (arg, cont)`,
/// and uses `cont`'s variable as the value of the App.
/// Once the enclosing Lam is done, the pending calls are wired up in encounter order - which respects
/// data dependencies, because operands are always rewritten before their users:
/// ```
/// lam   = k₀ (arg₀, cont₀)
/// cont₀ = k₁ (arg₁, cont₁)
/// ...
/// contₙ = rewritten body of lam
/// ```
/// A lifted call simply lives in the Lam whose body rewriting first reaches it.
/// Rewrites whose result mentions such a continuation variable are scoped to that Lam (see Conv::map) and
/// re-derived - and thus re-lifted - in sibling scopes.
class Conv : public RWPhase {
public:
    Conv(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// A lifted call `callee (arg, cont)` waiting to be wired into the body of the enclosing Lam.
    struct Pending {
        const Def* callee; ///< CPS callee `k: Cn [T, Cn U]`.
        const Def* arg;    ///< Rewritten argument - still without the continuation.
        Lam* cont;         ///< Fresh continuation receiving the result of the call.
    };

    /// RAII guard for rewriting the body of one mutable Lam:
    /// fences pending_, pushes a map scope for contaminated rewrites, and tracks whether lifting is legal here.
    class Scope {
    public:
        Scope(Conv& conv, bool liftable)
            : conv_(conv)
            , base_(conv.pending_.size())
            , old_liftable_(conv.liftable_) {
            conv_.push();
            conv_.liftable_ = liftable;
        }
        ~Scope() {
            assert(conv_.pending_.size() == base_ && "pending calls must have been wire()d");
            conv_.pop();
            conv_.liftable_ = old_liftable_;
        }

        size_t base() const { return base_; }

    private:
        Conv& conv_;
        size_t base_;
        bool old_liftable_;
    };

    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;

    /// Routes rewrites that mention scoped (continuation/binder) variables into the current Lam's map scope;
    /// everything else goes into the root map and is shared globally.
    using Rewriter::map;
    const Def* map(const Def*, const Def*) final;

    /// Converts direct-style `f: [a: A] → B` to `f_cps: Cn [a: A, Cn B]` and returns the `cps2ds_dep`-wrapped f_cps.
    const Def* convert(Lam*);
    /// Turns the call `k (arg, cont)` into a Pending entry and returns `cont`'s variable as call result.
    const Def* lift(const Def* k, const Def* arg, const App* old_app);
    /// Wires all Pending calls above @p base around @p body and returns the new body.
    const Def* wire(size_t base, const Def* body);

    fe::Vector<Pending> pending_;
    Vars scoped_;
    bool liftable_ = false;
};

} // namespace mim::plug::cps
