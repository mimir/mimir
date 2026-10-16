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

    auto [mem, val]      = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [mem.M 0, buffer.Buf (r, s, T)]
    auto [obr, obs, obT] = Axm::isa<buffer::Buf>(result_ty->proj(2, 1))->args<3>();
    auto [a_mem, buf]    = buffer::op_alloc(obr, obs, obT, mem)->projs<2>();
    // `compute` ignores the loop counters and writes the (loop-invariant) scalar `ins` everywhere.
    return build_pointwise(
        result_ty, a_mem, buf, val, s_out, *rn, "constant_fill",
        [](const DefVec&, const Def* ins, const Def* m) -> std::pair<const Def*, const Def*> { return {m, ins}; });
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    auto& w    = new_world();
    auto mr    = MapRedPost::of(rewrite(app->callee())->as<App>());
    auto ranks = mr.ranks();
    if (!ranks) {
        log().w("rank counts (nis/nps/Ro/Rn) of {} are not known at lowering time", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto impl = rewrite(old_world().annex<btensor::map_reduce_post_impl>());
    impl      = w.app(impl, mr.nis_nps);
    impl      = w.app(impl, {mr.To, mr.Tp, mr.Ro, w.lit_nat(ranks->rr)});
    impl      = w.app(impl, mr.shapes);
    impl      = w.app(impl, {mr.Tis, mr.Tps});
    impl      = w.app(impl, {mr.comb, mr.init, mr.post});
    impl      = w.app(impl, mr.map_out);

    auto [mem, dst, inputs, post_inputs]                 = rewrite(app->arg())->projs<4>();
    auto [read_inputs_ty, read_post_ty, write_output_ty] = impl->type()->as<Pi>()->dom()->projs<3>();

    // A `buffer.Buf` has its literal size-1 axes folded away, so the indices fold alike.
    auto reader = [&](const Def* type, const char* name, u64 num, const Def* rank, const Def* bounds, const Def* rs,
                      const Def* ss, const Def* maps, const Def* buffers) {
        auto fun          = w.mut_lam(type->as<Pi>())->set(name);
        auto [args, ret]  = fun->vars<2>();
        auto [m, indices] = args->projs<2>();
        auto values       = DefVec(num);
        for (u64 i = 0; i != num; ++i) {
            auto si = ss->proj(num, i);
            auto [mapped, coords]
                = op_affine_map(maps->proj(num, i), rs->proj(num, i), rank, bounds, si, indices, m)->projs<2>();
            auto [read, value] = buffer::op_read(mapped, buffers->proj(num, i), *Shape(coords).fold(si))->projs<2>();
            m                  = read;
            values[i]          = value;
        }
        fun->app(true, ret, {m, w.tuple(values)});
        return fun;
    };
    auto read_inputs = reader(read_inputs_ty, "readInputs", ranks->nis, mr.Rn, mr.Sr, mr.Ris, mr.Sis, mr.maps, inputs);
    auto read_post
        = reader(read_post_ty, "readPost", ranks->nps, mr.Ro, mr.So, mr.Rps, mr.Sps, mr.post_maps, post_inputs);

    auto write_output = w.mut_lam(write_output_ty->as<Pi>())->set("writeOutput");
    {
        auto [args, ret]                = write_output->vars<2>();
        auto [m, output, coords, value] = args->projs<4>();
        auto stored                     = buffer::op_write(m, output, *Shape(coords).fold(mr.So), value);
        write_output->app(true, ret, stored->proj(2, 0));
    }

    impl = w.app(impl, {read_inputs, read_post, write_output});
    return w.app(impl, {mem, dst});
}

const Def* LowerMapReduce::lower_broadcast(const App* app) {
    auto& w                   = new_world();
    auto callee               = app->callee()->as<App>(); // (broadcast {impl}) (s_in, s_out)
    auto [s_in, s_out]        = rewrite(callee->arg())->projs<2>();
    auto [op_mem, dst, input] = rewrite(app->arg())->projs<3>();
    auto result_ty            = rewrite(app->type()); // [mem.M 0, buffer.Buf (ro, so, T)]

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

    return build_pointwise(result_ty, op_mem, dst, input, s_out, *r_l, "broadcast", compute);
}

const Def* LowerMapReduce::lower_pad(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    // callee: pad {T, r} [s_in] [mode, lo, hi] [s_out]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [Tr, s_in, params, s_out] = c->uncurry_args<4>();
    auto [mode, lo, hi]            = params->projs<3>();
    auto [op_mem, dst, input, val] = rewrite(app->arg())->projs<4>();
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

    return build_pointwise(result_ty, op_mem, dst, w.tuple({input, val}), s_out, *r_l, "pad", compute);
}

const Def* LowerMapReduce::lower_concat(const App* app) {
    auto c = rewrite(app->callee())->as<App>();

    // callee: concat {T, nis, r} [ax] {Sis} [s_out]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [TnisR, ax, Sis, s_out] = c->uncurry_args<4>();
    auto [T, nis, r]             = TnisR->projs<3>();
    auto [op_mem, dst, op_is]    = rewrite(app->arg())->projs<3>();
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

    return build_pointwise(result_ty, op_mem, dst, op_is, s_out, rn, "concat", compute);
}

const Def* LowerMapReduce::lower_gather(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [Tr, shapes, dim]         = c->uncurry_args<3>();
    auto [T, r]                    = Tr->projs<2>();
    auto [s_src, s_idx]            = shapes->projs<2>();
    auto [op_mem, dst, input, idx] = rewrite(app->arg())->projs<4>();
    auto result_ty                 = rewrite(app->type());

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
    return build_pointwise(result_ty, op_mem, dst, w.tuple({input, idx}), s_idx, *r_l, "gather", compute);
}

const Def* LowerMapReduce::lower_scatter(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [Tr, shapes, dim]                     = c->uncurry_args<3>();
    auto [T, r]                                = Tr->projs<2>();
    auto [s_src, s_idx, s_updates]             = shapes->projs<3>();
    auto [op_mem, op_dst, input, idx, updates] = rewrite(app->arg())->projs<5>();
    auto result_ty                             = rewrite(app->type());

    auto r_l   = Lit::isa<u64>(r);
    auto dim_l = Lit::isa<u64>(dim);
    if (!r_l || !dim_l) {
        log().w("{} doesn't have lowering-time known rank/axis", app);
        return RWPhase::rewrite_imm_App(app);
    }

    auto [fun, call] = ds_splice(w.tuple({op_mem, op_dst, input, idx, updates}), result_ty, "scatter");
    auto [fun_mem, out_buf, in_buf, idx_buf, update_buf] = fun->var(2, 0)->projs<5>();
    const Def* cont                                      = fun->var(2, 1);

    const Def* acc            = w.tuple({buffer::op_copy(fun_mem, out_buf, in_buf), out_buf});
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
