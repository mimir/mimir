#pragma once

#include <mim/phase.h>

#include <mim/plug/mem/escape.h>

namespace mim::plug::btensor::phase {

/// Frees the buffers a function allocates but does not let escape; see mem::EscapeAnalysis.
///
/// A buffer only its home's own memory chain mentions is freed right after its last use there.
/// Any other is freed whenever control leaves its home.
class FreeLocals : public RWPhase {
private:
    class Escape : public mem::EscapeAnalysis {
    public:
        Escape(World& world)
            : mem::EscapeAnalysis(world, "btensor::FreeLocals::Escape") {}

    private:
        bool allocates(const App*) const final;
        const Def* hands_back(const App*) const final;
        bool is_handle(const Def*) const final;
    };

public:
    FreeLocals(World& world, flags_t annex)
        : RWPhase(world, annex, &escape_)
        , escape_(world) {}

private:
    bool analyze() final;
    const Def* rewrite_imm_App(const App*) final;

    /// Rebuilds @p arg with `buffer.free`s of the (old-world) @p allocs threaded through its memory @p mem.
    const Def* thread(const Def* arg, const Def* mem, const DefVec& allocs);

    Escape escape_;
    /// Old op or tail ↦ allocations to free right before it.
    DefMap<DefVec> frees_;
};

} // namespace mim::plug::btensor::phase
