#include "mim/plug/affine/phase/lower_index.h"

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>
#include <mim/plug/refly/refly.h>

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

    // Emits `%core.div.<op> (mem_, (x, c))` on the `Idx 0` carrier, advancing the threaded mem and yielding the value.
    auto div = [&](core::div op, const Def* x, const Def* c) -> const Def* {
        auto [m, v] = w.call(op, Defs{mem_, w.tuple({x, c})})->projs<2>();
        mem_        = m;
        return v;
    };

    // The affine index algebra is computed on the wide `Idx 0` carrier with wrap-around (`Mode::none`) arithmetic, so
    // that negation/subtraction are correct via two's complement; the boundary `%affine.map` casts in/out with
    // `%core.conv.u`.

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
        auto [x, c] = rewrite(app->arg())->projs<2>();
        switch (semiop.id()) {
            case affine::semiop::mul: {
                if (Axm::isa<refly::check>(c)) error("affine.op.mul called with non-constant second argument");
                // `c` is a `Nat` constant; reinterpret it on the `Idx 0` carrier.
                return w.call(core::wrap::mul, core::Mode::none, Defs{x, w.call<core::bitcast>(w.type_i64(), c)});
            }
            case affine::semiop::floordiv: {
                return div(core::div::udiv, x, w.call<core::bitcast>(w.type_i64(), c));
            }
            case affine::semiop::mod: {
                return div(core::div::urem, x, w.call<core::bitcast>(w.type_i64(), c));
            }
            case affine::semiop::ceildiv: {
                auto c_idx  = w.call<core::bitcast>(w.type_i64(), c);
                // ceildiv(x, c) = (x + (c - 1)) / c  (unsigned, on the Idx 0 carrier)
                auto c_1 = w.call(core::wrap::sub, core::Mode::none, Defs{c_idx, w.lit(w.type_i64(), 1)});
                return div(core::div::udiv, w.call(core::wrap::add, core::Mode::none, Defs{x, c_1}), c_idx);
            }
        }
    }

    // %affine.map f idxs mem ↦ widen idxs to `Idx 0`, inline f (advancing the threaded mem through any div), and narrow each
    // result back to its target `Idx (sout#j)`; returns `(mem', narrowed)`.
    if (Axm::isa<affine::map>(app)) {
        // Extract f/idxs/sout from the *old* callee; we inline f's body at this call site rather than rewriting it into a
        // standalone lam, since its body may reference the threaded mem (from the divs) and would otherwise be open.
        auto [mn, sinout, f, idxs] = app->callee()->as<App>()->uncurry_args<4>();
        auto [sin, sout]           = sinout->projs<2>();

        auto saved = mem_;
        mem_       = rewrite(app->arg()); // the `%affine.map`'s mem operand

        auto ins    = rewrite(idxs)->projs();
        auto lifted = w.tuple(DefVec(ins.size(), [&](size_t i) { return w.call(core::conv::u, w.lit_i64(), ins[i]); }));

        auto f_lam = f->isa_mut<Lam>();
        push();
        map(f_lam->var(), lifted);
        auto outs = rewrite(f_lam->body())->projs(); // «m; Idx 0»; div semiops advance mem_ here
        pop();

        auto sout_n   = rewrite(sout);
        auto narrowed = w.tuple(
            DefVec(outs.size(), [&](size_t j) { return w.call(core::conv::u, sout_n->proj(outs.size(), j), outs[j]); }));

        auto mem = mem_;
        mem_     = saved;
        return w.tuple({mem, narrowed});
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::affine::phase
