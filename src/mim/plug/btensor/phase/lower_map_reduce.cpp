#include "mim/plug/btensor/phase/lower_map_reduce.h"

#include <mim/axm.h>
#include <mim/def.h>
#include <mim/lam.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/btensor/btensor.h"

namespace mim::plug::btensor::phase {

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (Axm::isa<btensor::map_reduce_post>(app)) return lower_map_reduce_post(app);
    if (Axm::isa<btensor::broadcast>(app)) return lower_broadcast(app);
    if (Axm::isa<btensor::pad>(app)) return lower_pad(app);
    if (Axm::isa<btensor::concat>(app)) return lower_concat(app);
    if (Axm::isa<buffer::lit>(app)) return lower_buffer_lit(app);
    if (Axm::isa<btensor::gather>(app)) return lower_gather(app);
    if (Axm::isa<btensor::scatter>(app)) return lower_scatter(app);
    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerMapReduce::lower_buffer_lit(const App* app) {
    // `buffer.lit (r, s, T) (mem, val)` fills every elem with `val`. Emit a fill loop (via the same
    // pointwise scaffold as pad/concat) so the store never materializes as one giant literal array. A
    // non-literal rank has no static loop nest, so leave it for `buffer.lower_ptr`'s monolithic fallback.
    auto [r, s, T] = app->callee()->as<App>()->args<3>();
    auto s_out     = rewrite(s);
    auto rn        = Lit::isa<u64>(rewrite(r));
    if (!rn) return RWPhase::rewrite_imm_App(app);

    auto [mem, val] = rewrite(app->arg())->projs<2>();
    auto result_ty  = rewrite(app->type()); // [mem.M 0, buffer.Buf (r, s, T)]
    // `compute` ignores the loop counters and writes the (loop-invariant) scalar `ins` everywhere.
    return build_pointwise(
        result_ty, mem, val, s_out, *rn, "constant_fill",
        [](const DefVec&, const Def* ins, const Def* m) -> std::pair<const Def*, const Def*> { return {m, ins}; });
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    auto& w = new_world();
    auto mr = MapRedPost::of(rewrite(app->callee())->as<App>());

    // The final argument is `[mem, is, post_is]`; the result is `[mem, Buf]`.
    auto [op_mem, op_is, op_post_is] = rewrite(app->arg())->projs<3>();
    auto result_ty                   = rewrite(app->type()); // [mem.M 0, buffer.Buf (Ro, So, Tp)]

    auto ranks = mr.ranks();
    if (!ranks) {
        log().w("rank counts (nis/nps/Ro/Rn) of {} are not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto [nis_nat, nps_nat, ro, rr] = *ranks;
    auto nloops                     = ranks->nloops();
    auto n                          = w.lit_nat(nloops);

    // `[mem, is, post_is] → [mem, Buf]`, spliced via cps.cps2ds and applied to the op's (mem, is, post_is).
    auto [fun, call]                        = ds_splice(w.tuple({op_mem, op_is, op_post_is}), result_ty, "mapRedAff");
    auto [fun_mem, new_inputs, new_post_is] = fun->var(2, 0)->projs<3>();
    auto cont                               = fun->var(2, 1);

    // The op's SCHEDULE `sched` is a target-agnostic chooser over a loop-nest builder (canonically
    // a `tensor.mk_sched` value, selected in the frontend). This is the tensor→btensor boundary,
    // so bind it HERE to this target's algebra — `btensor.mr_nest` over the output buffer — then
    // build only the decision-free pieces: the fold step `cell` (read one element per input, call
    // the combiner) and the write-back `wb` (read the epilogue inputs, run `post`, store) — and
    // APPLY the nest to them. Unrolling, interchange and the row accumulator are inside the
    // builder: plain IR, not lowering behavior.
    auto i32 = w.type_i32();

    // Allocate the output buffer.
    auto [obr, obs, obT]  = Axm::isa<buffer::Buf>(result_ty->proj(2, 1))->args<3>();
    auto [a_mem, out_buf] = buffer::op_alloc(obr, obs, obT, fun_mem)->projs<2>();

    auto nest_args = w.tuple({mr.Ro, w.lit_nat(rr), mr.Sr, mr.To, result_ty->proj(2, 1)});
    auto nest      = w.app(w.app(mr.sched, w.app(w.annex<btensor::NestT>(), nest_args)),
                           w.app(w.annex<btensor::mr_nest>(), nest_args));

    // The bound nest dictates the exact `cell`/`wb` signatures (its [init, cell, wb] domain) —
    // building them from the VALUE's own type sidesteps any Arr/Sigma normalization asymmetry.
    auto sched_dom = nest->type()->as<Pi>()->dom();

    // The nest value's loop-vector components may appear as one «r; I32» value or flattened into r
    // separate I32 components (normalization decides) — index the domains verbatim either way.
    auto load_ivs = [&](Lam* l, u64 ndom, u64 pos, u64 cnt) {
        DefVec out(cnt);
        if (ndom == pos + cnt + 1) // flattened: cnt I32 scalars, then the continuation
            for (u64 d = 0; d < cnt; ++d)
                out[d] = l->var(ndom, pos + d);
        else
            for (u64 d = 0; d < cnt; ++d)
                out[d] = l->var(ndom, pos)->proj(cnt, d);
        return out;
    };

    auto sr = mr.Sr->projs(nloops);

    // cell: Cn [mem, To, «ro+rr; I32», Cn [mem, To]] — fold the elements at one loop vector.
    auto cdom = sched_dom->proj(3, 1)->as<Pi>()->dom();
    auto cn   = cdom->num_projs();
    auto cell = w.mut_con(cdom)->set("cell");
    {
        auto cm    = cell->var(cn, 0);
        auto cacc  = cell->var(cn, 1);
        auto ck    = cell->var(cn, cn - 1);
        auto iters = w.tuple(to_idxs(w, sr, load_ivs(cell, cn, 2, nloops)));
        auto cur   = cm;
        DefVec input_elems(nis_nat);
        for (u64 i = 0; i < nis_nat; ++i) {
            auto sis_i = mr.Sis->proj(nis_nat, i);
            auto [mc_mem, coords]
                = op_affine_map(mr.maps->proj(nis_nat, i), mr.Ris->proj(nis_nat, i), n, mr.Sr, sis_i, iters, cur)
                      ->projs<2>();
            auto [rd_mem, rd_val]
                = buffer::op_read(mc_mem, new_inputs->proj(nis_nat, i), *Shape(coords).fold(sis_i))->projs<2>();
            cur            = rd_mem;
            input_elems[i] = rd_val;
        }
        app_cps(cell, mr.comb, {cur, cacc, w.tuple(input_elems)}, ck);
    }

    // wb: Cn [mem, Buf, To, «ro+rr; I32», Cn [mem, Buf]] — epilogue + store for one folded cell,
    // threading the output buffer as the nest's write-back target. It receives the full loop
    // vector; only the leading `ro` output coordinates are read (the trailing reduction slots are
    // exhausted loop values and are replaced by zeros for `map_out`).
    auto wdom = sched_dom->proj(3, 2)->as<Pi>()->dom();
    auto wn   = wdom->num_projs();
    auto wb   = w.mut_con(wdom)->set("wb");
    {
        auto wm       = wb->var(wn, 0);
        auto wu       = wb->var(wn, 1);
        auto wv       = wb->var(wn, 2);
        auto wk       = wb->var(wn, wn - 1);
        auto wovs     = load_ivs(wb, wn, 3, nloops);
        auto wb_iters = to_idxs(w, Defs(sr).subspan(0, ro), Defs(wovs).subspan(0, ro));
        for (u64 j = 0; j < rr; ++j)
            wb_iters.emplace_back(w.call(core::conv::u, sr[ro + j], w.lit(i32, 0)));
        auto [wc_mem, write_coords]
            = op_affine_map(mr.map_out, mr.Ro, n, mr.Sr, mr.So, w.tuple(wb_iters), wm)->projs<2>();

        auto pcur = wc_mem;
        DefVec post_elems(nps_nat);
        for (u64 j = 0; j < nps_nat; ++j) {
            auto sps_j             = mr.Sps->proj(nps_nat, j);
            auto [pc_mem, pcoords] = op_affine_map(mr.post_maps->proj(nps_nat, j), mr.Rps->proj(nps_nat, j), mr.Ro,
                                                   mr.So, sps_j, write_coords, pcur)
                                         ->projs<2>();
            auto [prd_mem, p_val]
                = buffer::op_read(pc_mem, new_post_is->proj(nps_nat, j), *Shape(pcoords).fold(sps_j))->projs<2>();
            pcur          = prd_mem;
            post_elems[j] = p_val;
        }
        auto after_post            = mem::mut_con(mr.Tp)->set("afterPost");
        auto [post_mem, elem_post] = after_post->vars<2>();
        auto stored = buffer::op_write(obr, obs, obT, post_mem, wu, *Shape(write_coords).fold(mr.So), elem_post);
        after_post->app(true, wk, stored);
        app_cps(wb, mr.post, {pcur, wv, w.tuple(post_elems)}, after_post);
    }

    // Apply the nest; the output buffer is threaded through as the nest's write-back target and
    // yielded straight to the op's continuation.
    fun->app(true, w.app(nest, w.tuple({mr.init, cell, wb})), w.tuple({a_mem, out_buf, cont}));

    return call;
}

const Def* LowerMapReduce::lower_broadcast(const App* app) {
    auto& w              = new_world();
    auto callee          = app->callee()->as<App>(); // (broadcast {impl}) (s_in, s_out)
    auto [s_in, s_out]   = rewrite(callee->arg())->projs<2>();
    auto [op_mem, input] = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [mem.M 0, buffer.Buf (ro, so, T)]

    auto r_l = Lit::isa(s_out->arity());
    if (!r_l) {
        log().w("rank of {} is not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }

    // Non-size-1 input dims mirror the matching output index; size-1 dims are dropped from each buffer index.
    auto compute = [&](const DefVec& iters, const Def* in_buf, const Def* mem) -> std::pair<const Def*, const Def*> {
        auto idxs             = w.tuple(to_idxs(w, s_out->projs(*r_l), iters));
        auto [rd_mem, rd_val] = buffer::op_read(mem, in_buf, *Shape(idxs).fold(s_in))->projs<2>();
        return {rd_mem, rd_val};
    };

    return build_pointwise(result_ty, op_mem, input, s_out, *r_l, "broadcast", compute);
}

const Def* LowerMapReduce::lower_pad(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    // callee: pad {T, r} [s_in] [mode, lo, hi] [s_out]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [Tr, s_in, params, s_out] = c->uncurry_args<4>();
    auto [mode, lo, hi]            = params->projs<3>();
    auto [op_mem, input, val]      = rewrite(app->arg())->projs<3>();
    auto result_ty                 = rewrite(app->type()); // [mem.M 0, buffer.Buf (r, s_out, T)]

    auto r_l    = Lit::isa<u64>(Tr->proj(2, 1));
    auto mode_l = Lit::isa<u64>(mode);
    if (!r_l || !mode_l) {
        log().w("rank/mode of {} is not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto compute = [&](const DefVec& iters, const Def* ins, const Def* mem) -> std::pair<const Def*, const Def*> {
        auto [in_buf, fill]  = ins->projs<2>();
        auto [coords, valid] = pad_coords(iters, lo, s_in, *mode_l != 0);
        auto [rd_mem, elem]  = buffer::op_read(mem, in_buf, *Shape(w, coords).fold(s_in))->projs<2>();
        return {rd_mem, valid ? w.select(valid, elem, fill) : elem}; // constant: fill out-of-region cells with `val`
    };

    return build_pointwise(result_ty, op_mem, w.tuple({input, val}), s_out, *r_l, "pad", compute);
}

const Def* LowerMapReduce::lower_concat(const App* app) {
    auto c = rewrite(app->callee())->as<App>();

    // callee: concat {T, nis, r} [ax] {Sis} [s_out]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [TnisR, ax, Sis, s_out] = c->uncurry_args<4>();
    auto [T, nis, r]             = TnisR->projs<3>();
    auto [op_mem, op_is]         = rewrite(app->arg())->projs<2>();
    auto result_ty               = rewrite(app->type()); // [mem.M 0, buffer.Buf (r, s_out, T)]

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) {
        log().w("nis/rank/axis of {} are not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto nisn = *nis_l, rn = *r_l, axn = *ax_l;

    auto exts = concat_extents(Sis, nisn, rn, axn);
    if (!exts) {
        log().w("an extent of {} along the concat axis is not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto compute = [&](const DefVec& iters, const Def* ins, const Def* mem) -> std::pair<const Def*, const Def*> {
        const Def* cur = mem;
        auto read      = [&](u64 i, const Def* ax_idx) {
            auto coords = iters;
            coords[axn] = ax_idx;
            auto [rd_mem, rd_val]
                = buffer::op_read(cur, ins->proj(nisn, i), folded_idx(Sis->proj(nisn, i), coords))->projs<2>();
            cur = rd_mem;
            return rd_val;
        };
        auto result = concat_select(iters[axn], *exts, read);
        return {cur, result};
    };

    return build_pointwise(result_ty, op_mem, op_is, s_out, rn, "concat", compute);
}

const Def* LowerMapReduce::lower_gather(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [Tr, shapes, dim]    = c->uncurry_args<3>();
    auto [T, r]               = Tr->projs<2>();
    auto [s_src, s_idx]       = shapes->projs<2>();
    auto [op_mem, input, idx] = rewrite(app->arg())->projs<3>();
    auto result_ty            = rewrite(app->type());

    auto r_l   = Lit::isa<u64>(r);
    auto dim_l = Lit::isa<u64>(dim);
    if (!r_l || !dim_l) {
        log().w("{} doesn't have lowering-time known rank/axis", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto compute = [&](const DefVec& iters, const Def* ins, const Def* mem) -> std::pair<const Def*, const Def*> {
        auto [in_buf, idx_buf]   = ins->projs<2>();
        auto [idx_mem, selected] = buffer::op_read(mem, idx_buf, folded_idx(s_idx, iters))->projs<2>();
        auto src                 = iters;
        src[*dim_l]              = w.call<core::bitcast>(w.type_i64(), selected);
        auto [read_mem, val]     = buffer::op_read(idx_mem, in_buf, folded_idx(s_src, src))->projs<2>();
        return {read_mem, val};
    };
    return build_pointwise(result_ty, op_mem, w.tuple({input, idx}), s_idx, *r_l, "gather", compute);
}

const Def* LowerMapReduce::lower_scatter(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [Tr, shapes, dim]             = c->uncurry_args<3>();
    auto [T, r]                        = Tr->projs<2>();
    auto [s_src, s_idx, s_updates]     = shapes->projs<3>();
    auto [op_mem, input, idx, updates] = rewrite(app->arg())->projs<4>();
    auto result_ty                     = rewrite(app->type());

    auto r_l   = Lit::isa<u64>(r);
    auto dim_l = Lit::isa<u64>(dim);
    if (!r_l || !dim_l) {
        log().w("{} doesn't have lowering-time known rank/axis", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto [fun, call] = ds_splice(w.tuple({op_mem, input, idx, updates}), result_ty, "scatter");
    auto [fun_mem, in_buf, idx_buf, update_buf] = fun->var(2, 0)->projs<4>();
    const Def* cont                             = fun->var(2, 1);

    auto [obr, obs, obT]      = Axm::isa<buffer::Buf>(result_ty->proj(2, 1))->args<3>();
    auto [a_mem, out_buf]     = buffer::op_alloc(obr, obs, obT, fun_mem)->projs<2>();
    const Def* acc            = w.tuple({buffer::op_copy(a_mem, out_buf, in_buf), out_buf});
    auto current              = fun;
    auto iters                = affine::counting_nest(current, cont, acc, s_idx->projs(*r_l), "scatter");
    auto [loop_mem, loop_buf] = acc->projs<2>();

    auto [idx_mem, selected]  = buffer::op_read(loop_mem, idx_buf, folded_idx(s_idx, iters))->projs<2>();
    auto [update_mem, update] = buffer::op_read(idx_mem, update_buf, folded_idx(s_updates, iters))->projs<2>();
    auto dst                  = iters;
    dst[*dim_l]               = w.call<core::bitcast>(w.type_i64(), selected);
    auto write_mem            = buffer::op_write(update_mem, loop_buf, folded_idx(s_src, dst), update)->proj(2, 0);
    current->app(true, cont, w.tuple({write_mem, loop_buf}));
    return call;
}

} // namespace mim::plug::btensor::phase
