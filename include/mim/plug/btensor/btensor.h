#pragma once

#include <optional>

#include <mim/lam.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/btensor/autogen.h"

namespace mim::plug::btensor {

/// The curried meta arguments of a `tensor.map_reduce_post` or `btensor.map_reduce_post` callee (same layout).
struct MapRedPost {
    const Def *nis_nps, *meta, *shapes, *in_tys, *comb_init, *map_out, *maps_all;
    const Def *nis, *nps, *To, *Tp, *Ro, *Rn, *TSched, *So, *Sr, *sched;
    const Def *Tis, *Ris, *Sis, *Tps, *Rps, *Sps, *comb, *init, *post, *maps, *post_maps;

    static MapRedPost of(const App* callee) {
        auto [nis_nps, meta, shapes, in_tys, comb_init, map_out, maps_all] = callee->uncurry_args<7>();
        auto [nis, nps]                                                    = nis_nps->projs<2>();
        auto [To, Tp, Ro, Rn, TSched]                                      = meta->projs<5>();
        auto [So, Sr, sched]                                               = shapes->projs<3>();
        auto [Tis, Ris, Sis, Tps, Rps, Sps]                                = in_tys->projs<6>();
        auto [comb, init, post]                                            = comb_init->projs<3>();
        auto [maps, post_maps]                                             = maps_all->projs<2>();
        return {nis_nps, meta, shapes, in_tys, comb_init, map_out, maps_all, nis,      nps, To,
                Tp,      Ro,   Rn,     TSched, So,        Sr,      sched,    Tis,      Ris, Sis,
                Tps,     Rps,  Sps,    comb,   init,      post,    maps,     post_maps};
    }

    struct Ranks {
        u64 nis, nps, ro, rr;
        u64 nloops() const { return ro + rr; }
    };

    /// The literal input counts and loop ranks; `std::nullopt` unless all are literal and `Rn >= Ro`.
    std::optional<Ranks> ranks() const {
        auto nis_l = Lit::isa<u64>(nis);
        auto nps_l = Lit::isa<u64>(nps);
        auto ro_l  = Lit::isa<u64>(Ro);
        auto rn_l  = Lit::isa<u64>(Rn);
        if (!nis_l || !nps_l || !ro_l || !rn_l || *rn_l < *ro_l) return {};
        return Ranks{*nis_l, *nps_l, *ro_l, *rn_l - *ro_l};
    }
};

/// `affine.map @(m, n) @(s_in, s_out) f idxs as mem` with the address space `as` of @p mem ↦ `[mem', coords]`.
inline const Def* op_affine_map(const Def* f,
                                const Def* m,
                                const Def* n,
                                const Def* s_in,
                                const Def* s_out,
                                const Def* idxs,
                                const Def* mem) {
    auto& w = mem->world();
    auto a  = w.app(w.annex<affine::map>(), Defs{m, n});
    a       = w.app(a, Defs{s_in, s_out});
    a       = w.app(a, f);
    a       = w.app(a, idxs);
    a       = w.app(a, mem->type()->as<App>()->arg());
    return w.app(a, mem);
}

/// Calls the combiner/epilogue @p f from @p caller with @p args and the continuation @p ret.
/// Scalarize may have flattened @p f's domain `Cn [[args], Cn ret]` to `Cn [args…, Cn ret]`; the call matches either.
inline void app_cps(Lam* caller, const Def* f, DefVec args, const Def* ret) {
    auto& w = caller->world();
    if (f->type()->as<Pi>()->dom()->num_projs() == args.size() + 1) {
        args.emplace_back(ret);
        caller->app(true, f, w.tuple(args));
    } else {
        caller->app(true, f, w.tuple({w.tuple(args), ret}));
    }
}

/// A fresh `fun: Fn arg->type() → codom` named @p name, spliced into direct style as `cps.cps2ds_dep fun arg`.
/// @returns `{fun, call}`.
inline std::pair<Lam*, const Def*> ds_splice(const Def* arg, const Def* codom, std::string name) {
    auto& w   = arg->world();
    auto fun  = w.mut_fun(arg->type(), codom)->set(std::move(name));
    auto call = w.app(cps::op_cps2ds_dep(fun), arg);
    return {fun, call};
}

/// Converts the raw i64 loop counters @p iters into indices `Idx extents[d]`.
inline DefVec to_idxs(World& w, Defs extents, Defs iters) {
    return DefVec(iters.size(), [&](size_t d) { return w.call(core::conv::u, extents[d], iters[d]); });
}

/// The raw i64 loop counters @p iters as a buffer index into @p shape, with its literal size-1 axes folded away.
inline const Def* folded_idx(const Def* shape, Defs iters) {
    auto& w = shape->world();
    return *Shape(w, to_idxs(w, shape->projs(iters.size()), iters)).fold(shape);
}

/// Clamps the i64 @p x into `[0, hi]`.
inline const Def* clamp(const Def* x, const Def* hi) {
    auto& w = x->world();
    return w.call(core::extrema::smax, Defs{w.lit_i64(0), w.call(core::extrema::smin, Defs{x, hi})});
}

/// The read coordinates `o#d − lo#d` of a `pad` into `Idx s_in#d` - clamped to the nearest edge if @p replicate, or
/// else zeroed out of range - together with the all-axes in-range flag (`nullptr` if @p replicate).
inline std::pair<DefVec, const Def*> pad_coords(Defs iters, const Def* lo, const Def* s_in, bool replicate) {
    auto& w  = lo->world();
    auto i64 = w.type_i64();
    auto rn  = iters.size();
    DefVec coords(rn);
    DefVec valid;
    for (u64 d = 0; d < rn; ++d) {
        auto lo_d  = w.call<core::bitcast>(i64, lo->proj(rn, d));
        auto sin_d = w.call<core::bitcast>(i64, s_in->proj(rn, d));
        auto in_d  = w.call(core::wrap::sub, core::Mode::none, Defs{iters[d], lo_d});
        const Def* idx_i64;
        if (replicate) {
            idx_i64 = clamp(in_d, w.call(core::wrap::sub, core::Mode::none, Defs{sin_d, w.lit_i64(1)}));
        } else { // a single unsigned `<` covers both bounds since underflow wraps high
            auto v_d = w.call(core::icmp::ul, Defs{in_d, sin_d});
            valid.push_back(v_d);
            idx_i64 = w.select(v_d, in_d, w.lit_i64(0));
        }
        coords[d] = w.call(core::conv::u, s_in->proj(rn, d), idx_i64);
    }
    if (replicate) return {coords, nullptr};
    auto all_valid = valid.empty() ? w.lit_tt() : valid[0];
    for (u64 d = 1; d < valid.size(); ++d)
        all_valid = w.call(core::bit2::and_, w.lit_nat(2), w.tuple({all_valid, valid[d]}));
    return {coords, all_valid};
}

/// The literal extents of the `nis` input shapes @p Sis along the concat axis @p ax; `std::nullopt` if one is not
/// literal.
inline std::optional<fe::Vector<u64>> concat_extents(const Def* Sis, u64 nis, u64 r, u64 ax) {
    auto exts = fe::Vector<u64>(nis);
    for (u64 i = 0; i < nis; ++i)
        if (auto e = Lit::isa<u64>(Sis->proj(nis, i)->proj(r, ax)))
            exts[i] = *e;
        else
            return {};
    return exts;
}

/// The concat cell at the i64 axis coordinate @p o_ax: `read(i, loc)` of the input `i` owning it - the highest one
/// whose prefix offset is `≤ o_ax` - at the axis coordinate `loc` clamped into that input.
inline const Def* concat_select(const Def* o_ax, const fe::Vector<u64>& exts, auto read) {
    auto& w     = o_ax->world();
    auto off    = u64(0);
    auto read_i = [&](u64 i) {
        auto loc = w.call(core::wrap::sub, core::Mode::none, Defs{o_ax, w.lit_i64(off)});
        return read(i, clamp(loc, w.lit_i64(exts[i] - 1)));
    };
    auto result = read_i(0);
    for (u64 i = 1; i < exts.size(); ++i) {
        off += exts[i - 1];
        auto cond = w.call(core::icmp::uge, Defs{o_ax, w.lit_i64(off)});
        result    = w.select(cond, read_i(i), result);
    }
    return result;
}

/// A `[mem, dst, ins] → [mem, Buf]` fun spliced via `cps.cps2ds_dep` that loops over @p s_out carrying
/// `[mem, dst]` and writes `compute`'s element at the loop's (identity) output coordinates.
/// `compute(iters, ins, mem)` receives the raw i64 loop counters, the fun's inputs var, and the current mem; it
/// returns `{mem', elem}`.
inline const Def* build_pointwise(const Def* result_ty, // [mem.M 0, buffer.Buf (r, s_out, T)]
                                  const Def* mem,
                                  const Def* dst,
                                  const Def* ins,
                                  const Def* s_out,
                                  u64 rn,
                                  const std::string& name,
                                  auto compute) {
    auto& w                          = mem->world();
    auto [fun, call]                 = ds_splice(w.tuple({mem, dst, ins}), result_ty, name);
    auto [fun_mem, out_buf, fun_ins] = fun->var(2, 0)->projs<3>();
    const Def* cont                  = fun->var(2, 1);
    const Def* acc                   = w.tuple({fun_mem, out_buf});
    auto cur                         = fun;
    auto iters                       = affine::counting_nest(cur, cont, acc, s_out->projs(rn), name);
    auto [loop_mem, loop_buf]        = acc->projs<2>();

    std::pair<const Def*, const Def*> el = compute(iters, fun_ins, loop_mem);
    auto [el_mem, elem]                  = el;
    auto wr_mem = buffer::op_write(el_mem, loop_buf, folded_idx(s_out, iters), elem)->proj(2, 0);
    cur->app(true, cont, w.tuple({wr_mem, loop_buf}));
    return call;
}

} // namespace mim::plug::btensor
