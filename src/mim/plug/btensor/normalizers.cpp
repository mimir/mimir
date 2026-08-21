#include <mim/def.h>
#include <mim/world.h>

#include "mim/plug/btensor/btensor.h"

namespace mim::plug::btensor {

/// %btensor.map_reduce is %btensor.map_reduce_post without the epilogue and with the neutral schedule:
/// delegate with `post = %btensor.id` (so `Tp = To`), no epilogue inputs (`nps = 0`), no vector dim
/// (`vdim = Rn`, the out-of-range sentinel), and no unrolling.
/// The delegation lives in a normalizer (rather than a wrapping `lam`) so that call sites keep axm-style
/// implicit inference: the stuck axm application lets the checker solve the `{Tis, Ris, Sis}` holes from the
/// operands, whereas applying a `lam` would eagerly β-reduce `%buffer.Buf` over still-unsolved holes.
const Def* normalize_map_reduce(const Def*, const Def* c, const Def* arg) {
    auto& w = c->world();

    auto [nis, meta, shapes, TisRisSis, comb_init, map_out, maps] = c->as<App>()->uncurry_args<7>();
    auto [To, Ro, Rn]                                             = meta->projs<3>();
    auto [So, Sr]                                                 = shapes->projs<2>();
    auto [Tis, Ris, Sis]                                          = TisRisSis->projs<3>();
    auto [comb, init]                                             = comb_init->projs<2>();
    auto [mem, is]                                                = arg->projs<2>();

    auto sched = w.app(w.annex<btensor::mk_sched>(), {Rn, w.lit_nat_0()});
    auto post  = w.app(w.annex<btensor::id>(), To);
    auto unit  = w.tuple();

    auto op = w.annex<btensor::map_reduce_post>();
    op      = w.app(op, {nis, w.lit_nat_0()});
    op      = w.app(op, {To, To, Ro, Rn, sched->type()});
    op      = w.app(op, {So, Sr, sched});
    op      = w.app(op, {Tis, Ris, Sis, unit, unit, unit});
    op      = w.app(op, {comb, init, post});
    op      = w.app(op, map_out);
    op      = w.app(op, {maps, unit});
    return w.app(op, {mem, is, unit});
}

MIM_btensor_NORMALIZER_IMPL

} // namespace mim::plug::btensor
