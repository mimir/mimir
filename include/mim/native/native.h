#pragma once

#include "mim/driver.h"
#include "mim/phase.h"
#include "mim/target.h"

/// The target-independent phases of a native backend, parametrized by its Target.
/// The plugin `mach` hooks them with native::hook.
namespace mim::native {

/// Registers the Phase @p P for the annex @p A, constructed with the Target of the one loaded backend.
template<class A, class P>
void hook(Flags2Phases& phases) {
    fe::assert_emplace(phases, Annex::base<A>(), [](World& world) {
        return std::make_unique<P>(world, Annex::base<A>(), world.driver().target());
    });
}

/// A ClosedMutPhase over the functions of a scheduled world for one Target.
class FunPhase : public ClosedMutPhase<Lam> {
public:
    FunPhase(World& world, flags_t annex, const Target& target)
        : ClosedMutPhase(world, annex, true)
        , target_(target) {}

    const Target& target() const { return target_; }
    /// The arguments `-X <backend>:<arg>` of the Target's backend plugin.
    const fe::Vector<std::string>& target_args() { return driver().args(target_.name()); }

private:
    const Target& target_;
};

} // namespace mim::native
