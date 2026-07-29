#pragma once

#include <queue>

#include "mim/phase.h"

#include "mim/plug/clos/clos.h"
#include "mim/plug/mem/mem.h"

namespace mim::plug::clos::phase {

/// This pass lowers *typed closures* to *untyped closures*.
/// For details on typed closures, see ClosConv.
/// In general, untyped closure have the form `(pointer-to-environment, code)` with the following exceptions:
/// * Lam%s in callee-position should be λ-lifted and thus don't receive an environment.
/// * External and imported (not set) Lam%s also don't receive an environment.
///   They are appropriately η-wrapped by ClosConv.
/// * If the environment is of integer type, it's directly stored in the environment-pointer ("unboxed").
///   @note In theory this should work for other primitive types as well, but the LL backend does not handle the
///   required conversion correctly.
///
/// Further, first class continuations are rewritten to returning functions.
/// They receive `⊥` as a dummy continuation.
/// Therefore Clos2SJLJ should have taken place prior to this pass.
///
/// This pass will heap-allocate ClosKind::esc closures and stack-allocate everything else.
/// These annotations are introduced by LowerTypedClosPrep.
///
/// The rewrite carries a *mem token* along each function body (LowerTypedClos::lvm_ / LowerTypedClos::lcm_) so
/// that the `%mem.alloc`/`%mem.store` it inserts for boxed environments are threaded into the mem chain.
/// This stateful, order-sensitive threading is why LowerTypedClos::rewrite is overridden as a whole rather than
/// via the per-node hooks.
/// A converted Lam's body is enqueued and rewritten later, seeded with that body's own initial mem token.
class LowerTypedClos : public RWPhase {
public:
    LowerTypedClos(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// A pending body rewrite: the initial mem tokens (old @c lvm, new @c lcm) plus the old and new Lam.
    struct Todo {
        const Def* lvm;
        const Def* lcm;
        Lam* old_lam;
        Lam* new_lam;
    };

    void start() override;
    const Def* rewrite(const Def* def) final;

    /// Describes how the environment should be treated.
    enum Mode {
        Box = 0, ///< Environment is boxed (default).
        Unbox,   ///< Environment is of primitive type (currently `iN`s) and directly stored in the pointer.
        No_Env   ///< Lambda has no environment (lifted, top-level).
    };

    /// Create a new Lam stub.
    /// @p adjust_bb_type is true if the @p lam should be rewritten to a returning function.
    Lam* make_stub(Lam* lam, Mode mode, bool adjust_bb_type);

    /// Pointer type used to represent environments.
    const Def* env_type() { return new_world().call<mem::Ptr0>(new_world().sigma()); }

    std::queue<Todo> worklist_;

    const Def* dummy_ret_ = nullptr; ///< dummy return continuation
    bool converting_      = false;   ///< `false` while bootstrapping annexes; `true` once actually converting.

    /// @name memory-tokens
    /// The mem token threaded through the body currently being rewritten.
    ///@{
    const Def* lvm_ = nullptr; ///< Last visited memory token.
    const Def* lcm_ = nullptr; ///< Last created memory token.
    ///@}
};

} // namespace mim::plug::clos::phase
