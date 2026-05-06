#pragma once

#include <queue>

#include "mim/phase.h"

#include "mim/plug/clos/clos.h"
#include "mim/plug/mem/mem.h"

namespace mim::plug::clos {

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
class LowerTypedClos : public Phase {
public:
    LowerTypedClos(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override;

private:
    using StubQueue = std::queue<std::tuple<const Def*, const Def*, Lam*>>;

    /// Recursively rewrites a Def.
    const Def* rewrite(const Def* def);

    /// Describes how the environment should be treated.
    enum Mode {
        Box = 0, //< Environment is boxed (default).
        Unbox,   //< Environments is of primitive type (currently `iN`s) and directly stored in the pointer.
        No_Env   //< Lambda has no environment (lifted, top-level).
    };

    /// Create a new Lam stub.
    /// @p adjust_bb_type is true if the @p lam should be rewritten to a returning function.
    Lam* make_stub(Lam* lam, enum Mode mode, bool adjust_bb_type);

    /// @name Helpers
    ///@{
    /// wrapper around old2new_
    const Def* map(const Def* old_def, const Def* new_def) { return old2new_[old_def] = new_def; }
    Def* map(const Def* old_def, Def* new_def) {
        old2new_[old_def] = new_def;
        return new_def;
    }

    /// Pointer type used to represent environments
    const Def* env_type() {
        auto& w = world();
        return w.call<mem::Ptr0>(w.sigma());
    }
    ///@}

    Def2Def old2new_;
    StubQueue worklist_;

    const Def* dummy_ret_ = nullptr; //< dummy return continuation

    /// @name memory-tokens
    ///@{
    const Def* lvm_; //< Last visited memory token
    const Def* lcm_; //< Last created memory token
    ///@}

    std::vector<Lam*> new_externals_;
};

} // namespace mim::plug::clos
