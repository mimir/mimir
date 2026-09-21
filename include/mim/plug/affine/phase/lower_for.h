#pragma once

#include <mim/phase.h>

namespace mim::plug::affine::phase {

/// Lowers the for axm to actual control flow in CPS.
/// It basically mimics this implementation:
/// ```
/// con affine.For_impl
///     {m n: Nat, Ts: «n; *»}
///     (begin: Idx m, end: Idx m, step: Idx m, init: «i: n; Ts#i»,
///             body: Cn [iter: Idx m, acc: «i: n; Ts#i», yield: Cn «i: n; Ts#i»],
///             exit: Cn «i: n; Ts#i»
///     ) =
///     con head(iter: Idx m, acc: «i: n; Ts#i») =
///         con new_body() = body (iter, acc, cn acc: «i: n; Ts#i» =
///             let iter2 = core.wrap.add core.mode.nsuw (iter, step);
///             head (iter2, acc));
///         con new_exit() = exit (acc);
///         (new_exit, new_body)#(core.icmp.ul (iter, end)) ();
///     head(begin, init);
/// ```
/// However, we merge `init`/`acc` into the signature, as it may contain a `mem`.
/// In this case we have to equip `new_body`/`new_exit` with a `mem` as well.
class LowerFor : public RWPhase {
public:
    LowerFor(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::affine::phase
