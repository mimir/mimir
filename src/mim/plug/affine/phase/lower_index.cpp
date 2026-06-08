#include "mim/plug/affine/phase/lower_index.h"

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/core/core.h>

#include "mim/plug/affine/affine.h"

namespace mim::plug::affine::phase {

const Def* LowerIndex::rewrite(const Def* def) {
    // The opaque affine index type lowers to the wide `Idx 0` (i64) carrier.
    if (Axm::isa<affine::index>(def)) return new_world().type_i64();
    return RWPhase::rewrite(def);
}

const Def* LowerIndex::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    auto& w = new_world();

    // The affine index algebra is computed on the wide `Idx 0` carrier with wrap-around (`Mode::none`) arithmetic, so that
    // negation/subtraction are correct via two's complement; the boundary `%affine.map` casts in/out with `%core.conv.u`.

    // %affine.constant n ↦ the `Nat` n reinterpreted as an `Idx 0`.
    if (Axm::isa<affine::constant>(app)) return w.call<core::bitcast>(w.type_i64(), rewrite(app->arg()));

    if (auto op = Axm::isa<affine::op>(app)) {
        switch (op.id()) {
            case affine::op::add: {
                auto [a, b] = rewrite(app->arg())->projs<2>();
                return w.call(core::wrap::add, core::Mode::none, Defs{a, b});
            }
            case affine::op::sub: {
                auto [a, b] = rewrite(app->arg())->projs<2>();
                return w.call(core::wrap::sub, core::Mode::none, Defs{a, b});
            }
            case affine::op::neg: {
                auto a = rewrite(app->arg());
                return w.call(core::wrap::sub, core::Mode::none, Defs{w.lit(w.type_i64(), 0), a});
            }
            case affine::op::mul: {
                error("affine.op.mul should have been rewritten to affine.semiop.mul and then to core.mul");
            }
        }
    }

    if (auto semiop = Axm::isa<affine::semiop>(app)) {
        switch (semiop.id()) {
            case affine::semiop::mul: {
                auto [a, c] = rewrite(app->arg())->projs<2>();
                // `c` is a `Nat` constant; reinterpret it on the `Idx 0` carrier.
                return w.call(core::wrap::mul, core::Mode::none, Defs{a, w.call<core::bitcast>(w.type_i64(), c)});
            }
            case affine::semiop::ceildiv:
            case affine::semiop::floordiv:
            case affine::semiop::mod:
                error("affine: lowering of semiop `{}` is not yet implemented (needs %core.div with a %mem.M token)",
                      app);
        }
    }

    // %affine.map f idxs ↦ widen idxs to `Idx 0`, apply f, narrow each result back to its target `Idx (sout#j)`.
    if (Axm::isa<affine::map>(app)) {
        auto f      = rewrite(app->callee()->as<App>()->arg()); // «n; Idx 0» → «m; Idx 0»
        auto idxs   = rewrite(app->arg());                      // «n; Idx (sin#i)»
        auto res_ty = rewrite(app->type());                     // «m; Idx (sout#j)»

        auto ins    = idxs->projs();
        auto lifted = w.tuple(
            DefVec(ins.size(), [&](size_t i) { return w.call(core::conv::u, w.lit_i64(), ins[i]); }));

        auto outs = w.app(f, lifted)->projs(); // «m; Idx 0»
        return w.tuple(DefVec(outs.size(), [&](size_t j) {
            // `res_ty#j` is `Idx (sout#j)`, i.e. `%Idx sout#j`; recover the destination size for `%core.conv.u`.
            auto sout_j = res_ty->proj(outs.size(), j)->as<App>()->arg();
            return w.call(core::conv::u, sout_j, outs[j]);
        }));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::affine::phase
