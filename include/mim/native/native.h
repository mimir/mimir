#pragma once

#include <mim/phase.h>
#include <mim/target.h>

/// The target-independent phases of a native backend, parametrized by its Target.
/// A backend plugin links the static library `mim_native` and hooks them with native::hook.
namespace mim::native {

/// Registers the Phase @p P, constructed with @p target, for the annex @p A.
template<class A, class P>
void hook(Flags2Phases& phases, const Target& target) {
    fe::assert_emplace(phases, Annex::base<A>(),
                       [&target](World& world) { return std::make_unique<P>(world, Annex::base<A>(), target); });
}

/// A ClosedMutPhase over the functions of a scheduled world for one Target.
class FunPhase : public ClosedMutPhase<Lam> {
public:
    FunPhase(World& world, flags_t annex, const Target& target)
        : ClosedMutPhase(world, annex, true)
        , target_(target) {}

    const Target& target() const { return target_; }

private:
    const Target& target_;
};

} // namespace mim::native
