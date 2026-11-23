#pragma once

#include <deque>

#include <fe/vector.h>

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
    /// The chain being planned.
    struct Chain;

    /// Skips the whole phase if the program already contains an explicit `gpu.init`.
    // TODO: consider different solution to gpu.init vs gpu.auto_init problem
    void start() final;
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_map_reduce_post(const App*);

    /// The lowerable call producing @p def, unless it is already lowered or excluded from @p chain.
    const App* chainable(const Def* def, const Chain& chain);
    /// `nullptr` unless @p app can become a kernel (see the `plan_*` below).
    Call* plan(const App* app, Chain&);
    /// `nullptr` unless @p app has lowering-time known ranks, grid shape, combiner and epilogue.
    Call* plan_map_reduce(const App* app, Chain&);
    /// `nullptr` unless @p app has lowering-time known input count, rank, axis and shapes.
    Call* plan_concat(const App* app, Chain&);
    /// `nullptr` unless @p app is constant-mode with lowering-time known rank, shapes and a literal fill value.
    Call* plan_pad(const App* app, Chain&);
    void plan_inputs(Call&, bool post, const Def* group, Chain&);
    /// Marks every producer chained into @p call as absorbed and rewrites the remaining host inputs into @p hosts.
    void commit(Call& call, DefVec& hosts, DefMap<size_t>& host_index, fe::Vector<nat_t>& host_readers);
    /// Launches @p call's producers and then its own kernel; @returns the device pointer to its result.
    const Def* emit(Call& call, Session&);

    DefMap<nat_t> uses_;
    /// Calls whose result stays on the device; reached through the mem chain they only pass `mem` and `dst` on.
    DefSet absorbed_;
};

} // namespace mim::plug::gpu::phase
