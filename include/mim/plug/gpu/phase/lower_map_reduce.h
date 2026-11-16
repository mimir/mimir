#pragma once

#include <deque>

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class LowerMapReduce : public RWPhase {
public:
    using Super = RWPhase;

    LowerMapReduce(World& world, flags_t annex)
        : Super(world, annex) {}

private:
    /// A lowerable `btensor.map_reduce_post`, `btensor.concat` or `btensor.pad` together with the producers chained
    /// into it.
    struct Call;
    /// The open GPU session a chain of launches is emitted into.
    struct Session;

    /// Skips the whole phase if the program already contains an explicit `gpu.init`.
    // TODO: consider different solution to gpu.init vs gpu.auto_init problem
    void start() final;
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_map_reduce_post(const App*);

    /// The lowerable call whose buffer result is @p def, if only the reading call consumes it.
    const App* chainable(const Def* def);
    /// `nullptr` unless @p app can become a kernel (see the `plan_*` below) taking fewer than
    /// Flags::scalarize_threshold arguments.
    Call* plan(const App* app, std::deque<Call>&);
    /// `nullptr` unless @p app has lowering-time known ranks, grid shape, combiner and epilogue.
    Call* plan_map_reduce(const App* app, std::deque<Call>&);
    /// `nullptr` unless @p app has lowering-time known input count, rank, axis and shapes.
    Call* plan_concat(const App* app, std::deque<Call>&);
    /// `nullptr` unless @p app is constant-mode with lowering-time known rank, shapes and a literal fill value.
    Call* plan_pad(const App* app, std::deque<Call>&);
    void plan_inputs(Call&, bool post, const Def* group, std::deque<Call>&);
    /// Marks every producer chained into @p call as absorbed and rewrites the remaining host inputs into @p hosts.
    void commit(Call& call, DefVec& hosts);
    /// Launches @p call's producers and then its own kernel; @returns the device pointer to its result.
    const Def* emit(const Call& call, Session&);

    DefMap<nat_t> uses_;
    /// Calls whose result stays on the device; reached through the mem chain they only pass `mem` and `dst` on.
    DefSet absorbed_;
};

} // namespace mim::plug::gpu::phase
