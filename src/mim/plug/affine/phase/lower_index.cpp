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
                fe::throwf(
                    "`%affine.op.mul` should have been rewritten to `%affine.semiop.mul` and then to `%core.mul`");
            }
        }
    }

    if (auto semiop = Axm::isa<affine::semiop>(app)) {
        auto [x, c] = rewrite(app->arg())->projs<2>();
        switch (semiop.id()) {
            case affine::semiop::mul: {
                if (Axm::isa<refly::check>(c))
                    fe::throwf("`%affine.semiop.mul` called with non-constant second argument");
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
                auto c_idx = w.call<core::bitcast>(w.type_i64(), c);
                // ceildiv(x, c) = (x + (c - 1)) / c  (unsigned, on the Idx 0 carrier)
                auto c_1 = w.call(core::wrap::sub, core::Mode::none, Defs{c_idx, w.lit(w.type_i64(), 1)});
                return div(core::div::udiv, w.call(core::wrap::add, core::Mode::none, Defs{x, c_1}), c_idx);
            }
        }
    }

    // Row-major suffix-product strides of a shape `s` («n; Nat»): `strides#k = ∏_{j>k} s#j` (on `Idx 0`, via
    // `%core.nat`).
    auto strides = [&](const Def* s, size_t n) {
        DefVec str(n);
        if (n) str[n - 1] = w.lit_nat(1);
        for (size_t k = n - 1; k-- != 0;)
            str[k] = w.call(core::nat::mul, Defs{str[k + 1], s->proj(n, k + 1)});
        for (size_t k = 0; k != n; ++k)
            str[k] = w.call<core::bitcast>(w.type_i64(), str[k]);
        return str;
    };

    // %affine.linearize (idxs, s) ↦ Σ_k idxs#k · strides#k  (on the `Idx 0` carrier).
    if (Axm::isa<affine::linearize>(app)) {
        auto [idxs, s] = rewrite(app->arg())->projs<2>();
        auto xs        = idxs->projs();
        auto str       = strides(s, xs.size());
        const Def* lin = w.lit(w.type_i64(), 0);
        for (size_t k = 0; k != xs.size(); ++k) {
            auto term = w.call(core::wrap::mul, core::Mode::none, Defs{xs[k], str[k]});
            lin       = w.call(core::wrap::add, core::Mode::none, Defs{lin, term});
        }
        return lin;
    }

    // %affine.delinearize (lin, s) ↦ (lin floordiv strides#d) mod s#d for each d  (on the `Idx 0` carrier).
    if (Axm::isa<affine::delinearize>(app)) {
        auto [lin, s] = rewrite(app->arg())->projs<2>();
        auto m        = s->num_projs();
        auto str      = strides(s, m);
        return w.tuple(DefVec(m, [&](size_t d) {
            auto q = div(core::div::udiv, lin, str[d]);
            return div(core::div::urem, q, w.call<core::bitcast>(w.type_i64(), s->proj(m, d)));
        }));
    }

    // %affine.map f idxs mem ↦ widen idxs to `Idx 0`, inline f (advancing the threaded mem through any div), and narrow
    // each result back to its target `Idx (sout#j)`; returns `(mem', narrowed)`.
    if (Axm::isa<affine::map>(app)) {
        // Extract f/idxs/sout from the *old* callee; we inline f's body at this call site rather than rewriting it into
        // a standalone lam, since its body may reference the threaded mem (from the divs) and would otherwise be open.
        auto [mn, sinout, f, idxs] = app->callee()->as<App>()->uncurry_args<4>();
        auto [sin, sout]           = sinout->projs<2>();

        auto _   = Restore(mem_);
        auto mem = rewrite(app->arg()); // the `%affine.map`'s mem operand

        auto ins    = rewrite(idxs)->projs();
        auto lifted = w.tuple(DefVec(ins.size(), [&](size_t i) { return w.call(core::conv::u, w.lit_i64(), ins[i]); }));

        auto f_lam = f->isa_mut<Lam>();

        Lam* idx_map_lam = nullptr;
        Lam* rw_idx_lam  = nullptr;
        if (auto idx_lam = lookup(f_lam)) {
            if (auto idx_lam_mut = idx_lam->isa_mut<Lam>();
                idx_lam_mut && idx_lam_mut->num_vars() == 2 && idx_lam_mut->var(0)->type() == mem->type())
                // completely rewritten, great!
                idx_map_lam = idx_lam->as_mut<Lam>();
            else
                // was rewritten as part of an annex, probably.. so we need to rewrite it again adding mem.
                rw_idx_lam = idx_lam->as_mut<Lam>();
        }
        if (!idx_map_lam) {
            auto lam_pi = rewrite(f_lam->type())->as<Pi>();

            idx_map_lam = w.mut_lam(w.pi({mem->type(), lam_pi->dom()}, {mem->type(), lam_pi->codom()}));
            map(f_lam, idx_map_lam);

            push();
            map(f_lam->var(), idx_map_lam->var(1));
            for (size_t i = 0; i != f_lam->num_vars(); ++i)
                map(f_lam->var(i), idx_map_lam->var(1)->proj(f_lam->num_vars(), i));
            mem_ = idx_map_lam->var(0);

            auto get_body = [&]() -> const Def* {
                if (rw_idx_lam) return rw_idx_lam->reduce_body(idx_map_lam->var(1));
                return rewrite(f_lam->body());
            };
            idx_map_lam->set(true, w.tuple({mem_, get_body()}))->set(f_lam->dbg_key());
            pop();
        }

        auto outs = w.app(idx_map_lam, {mem, lifted});

        auto sout_n   = rewrite(sout);
        auto narrowed = w.tuple(DefVec(sout_n->num_projs(), [&](size_t j) {
            return w.call(core::conv::u, sout_n->proj(j), outs->proj(2, 1)->proj(j));
        }));

        return w.tuple({outs->proj(0), narrowed});
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::affine::phase
