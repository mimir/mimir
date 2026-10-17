#pragma once

#include <mim/nest.h>

#include "mim/plug/mem/escape_analysis.h"

namespace mim::plug::mem::phase {

/// Turns a `mem.alloc` that does not escape its function (see mem::EscapeAnalysis) into a `mem.slot`.
///
/// The slot's continuation receives the remainder of the alloc's home.
/// Only allocations of at most `-X mem:heap2stack=<n>` elements in address space 0 qualify; `0` disables this phase.
/// A slot lives until its function returns, so an alloc in a loop of its function stays on the heap.
class Heap2Stack : public RWPhase {
public:
    static constexpr nat_t Default_Max_Elems = 1024;

    Heap2Stack(World& world, flags_t annex)
        : RWPhase(world, annex, &escape_)
        , escape_(world) {}

private:
    bool analyze() final;
    const Def* rewrite_imm_App(const App*) final;

    nat_t max_elems();
    /// The number of scalars in @p type, saturated at @p max + 1; `std::nullopt` if not statically known.
    static std::optional<nat_t> num_elems(const Def* type, nat_t max);
    static bool in_loop(const Nest::Node*);

    EscapeAnalysis escape_;
    /// Old tail ↦ the allocs on its chain to turn into slots, in program order.
    DefMap<fe::Vector<const App*>> slots_;
};

} // namespace mim::plug::mem::phase
