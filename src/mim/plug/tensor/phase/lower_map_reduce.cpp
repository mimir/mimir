#include "mim/plug/tensor/phase/lower_map_reduce.h"

#include <numeric>
#include <optional>

#include <mim/def.h>
#include <mim/lam.h>

#include <mim/util/types.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/btensor/btensor.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/tensor/phase/constraints.h"
#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

const Def* as_array(const Def* tensor) { return tensor->world().struc(tensor)->proj(2, 1); }

const Def* as_tensor(const Def* type, const Def* array) {
    auto& w        = array->world();
    auto [r, s, T] = *TensorType::isa(type);
    auto indices   = w.wrap(w.tuple({r, s, w.bot(T)}));
    return w.name(type, w.tuple({indices, array}));
}

const Def* undef_tensor(const Def* type) {
    auto [r, s, T] = *TensorType::isa(type);
    return as_tensor(type, type->world().bot(type->world().arr(s, T)));
}

} // namespace

const Def* LowerMapReduce::rec_broadcast(const Def* s_in, const Def* s_out, const Def* input, u64 r, u64 i) {
    auto& w = new_world();
    // Base case: all dimensions have been processed; `input` is the final scalar.
    if (i == r) return input;

    auto s_in_ri = s_in->proj(r, i), s_out_ri = s_out->proj(r, i);
    log().d("broadcast dimension {} of {}: {} → {}, input = {}: {}", i, r, s_in_ri, s_out_ri, input, input->type());

    if (s_in_ri == s_out_ri) {
        if (auto s_in_lit = Lit::isa<u64>(s_in_ri)) {
            DefVec inputs(*s_in_lit,
                          [&](size_t j) { return rec_broadcast(s_in, s_out, input->proj(*s_in_lit, j), r, i + 1); });
            return w.tuple(inputs);
        } else {
            // TODO: we could probably support non-literal sizes as well, but we would need to generate loops to copy
            // the data instead of just packing it.
            log().w("dimension {} has equal but non-literal extent: {}", i, s_in_ri);
            return nullptr;
        }
    }

    if (auto s_in_lit = Lit::isa<u64>(s_in_ri); s_in_lit && *s_in_lit == 1) {
        log().d("dimension {}: packing the size-1 input to {}", i, s_out_ri);
        return w.pack(s_out_ri, rec_broadcast(s_in, s_out, input, r, i + 1));
    }

    log().w("cannot broadcast dimension {}: {} → {}", i, s_in_ri, s_out_ri);
    return nullptr;
}

const Def* LowerMapReduce::lower_broadcast(const App* app) {
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [s_in, s_out, input] = arg->projs<3>();
    auto callee               = c->as<App>();
    auto [T, r]               = callee->args<2>();
    log().d("lower broadcast: input = {}: {}, T = {}, r = {}, s_in = {}, s_out = {}", input, input->type(), T, r, s_in,
            s_out);

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        log().w("rank {} of {} is not known at lowering time", r, app);
        return nullptr;
    }
    // r_nat will never be 0, as we would have normalized this case away already
    if (s_in == s_out) return input;

    auto result = rec_broadcast(s_in, s_out, as_array(input), *r_nat, 0);
    log().d("broadcast result: {}", result);
    return result ? as_tensor(rewrite(app->type()), result) : nullptr;
}

const Def* LowerMapReduce::lower_map_reduce(const App* app) {
    // meta arguments:
    // * nis = in-count, nps = epilogue-input count (nat)
    // * To = accumulator type, Tp = out-element type (post: Fn [To, «nps; Tps»] → Tp), Ro = #output loops =
    //   result rank, Rn = #loops in total
    // * So = result shape (Ro*nat)
    // * Sr = the full loop bounds Rn*nat: the leading Ro are the output-loop bounds, the trailing Rn - Ro the
    // reductions
    // * Tis/Ris/Sis, Tps/Rps/Sps = (epilogue) input types/ranks/shapes
    // arguments:
    // * f = combination function (CPS), init = accumulator init, post = per-output-cell epilogue (CPS),
    //   applied to the folded accumulator and the epilogue elements right before the write-back
    // * map_out = affine map from the Rn loop vector to the Ro write coordinates in the result «So» (the reduction
    //             part is not in scope at write-back, so map_out must depend only on the leading Ro output indices)
    // * maps = per-input affine map from the Rn loop vector to the input's read coordinates
    // * post_maps = per-epilogue-input affine map from the Ro output-cell (write) coordinates to its read coordinates
    // * is, post_is = input tensors
    auto& w     = new_world();
    auto c      = rewrite(app->callee())->as<App>();
    auto inputs = rewrite(app->arg());
    auto type   = rewrite(app->type());

    auto mr = btensor::MapRedPost::of(c);
    auto Ro = mr.Ro, Sr = mr.Sr, So = mr.So;

    auto ranks = mr.ranks();
    if (!ranks) {
        log().w("rank counts (nis/nps/Ro/Rn) of {} are not known at lowering time", app);
        return nullptr;
    }
    auto [nis_nat, nps_nat, ro, rr] = *ranks;
    auto nloops                     = ranks->nloops();   // length of the full loop vector (= length of Sr)
    auto n                          = w.lit_nat(nloops); // passed as the affine maps' domain length

    // the affine maps below take each input's rank as their domain length, so it must be literal
    if (!lit_projs(mr.Ris, nis_nat) || !lit_projs(mr.Rps, nps_nat)) {
        log().w("the input ranks of {} are not known at lowering time", app);
        return nullptr;
    }

    // The result coordinates of `affine.map @(m, n) @(sin, sout) f idxs mem`, dropping the returned mem.
    // We invent a fresh `⊥ : mem.M 0` for the mem operand here; real mem threading is wired up later by `add_mem`.
    auto mem0       = w.call<mem::M>(0);
    auto affine_map = [&](const Def* f, const Def* m, const Def* n, const Def* sin, const Def* sout, const Def* idxs) {
        return btensor::op_affine_map(f, m, n, sin, sout, idxs, w.bot(mem0))->proj(2, 1);
    };

    try {
        auto fun    = w.mut_fun(inputs->type(), type)->set("mapRed");
        auto ds_fun = cps::op_cps2ds_dep(fun)->set("dsFun");
        auto call   = w.app(ds_fun, inputs)->set("call");

        auto [new_inputs, cont]    = fun->vars<2>();
        auto [new_is, new_post_is] = new_inputs->set("is")->projs<2>();
        auto sr                    = Sr->projs(nloops);

        // Outer (parallel) loops over the leading Ro bounds of `Sr`, collecting the output iteration indices.
        const Def* acc   = undef_tensor(type);
        auto current_mut = fun;
        auto raw_out     = affine::counting_nest(current_mut, cont, acc, Defs(sr).subspan(0, ro), "forOut");
        auto out_iters   = btensor::to_idxs(w, Defs(sr).subspan(0, ro), raw_out);
        auto wb_matrix   = acc;

        // Write-back: run the `post` epilogue on the accumulated element, then narrow the result into the
        // output at the affine write coordinates `map_out`.
        // map_out takes the full (Ro+Rr) loop vector, but the reduction loops have already been folded away here, so we
        // pass 0 for those slots; map_out must depend only on the leading Ro output indices.
        auto write_back    = w.mut_con(mr.To)->set("writeBack");
        auto element_final = write_back->var();
        DefVec wb_iters    = out_iters;
        for (u64 j = 0; j < rr; ++j)
            wb_iters.emplace_back(w.call(core::conv::u, sr[ro + j], w.lit_i64(0)));
        auto write_coords = affine_map(mr.map_out, Ro, n, Sr, So, w.tuple(wb_iters)); // «Ro; Idx (So#k)»

        // Read one element from each epilogue input at its post_maps-mapped output-cell coordinates.
        DefVec post_elements(nps_nat, [&](size_t j) {
            auto sps_j = mr.Sps->proj(nps_nat, j);
            auto coords
                = affine_map(mr.post_maps->proj(nps_nat, j), mr.Rps->proj(nps_nat, j), Ro, So, sps_j, write_coords);
            return w.extract(as_array(new_post_is->proj(nps_nat, j)), coords);
        });

        auto after_post = w.mut_con(mr.Tp)->set("afterPost");
        after_post->app(true, cont, as_tensor(type, w.insert(as_array(wb_matrix), write_coords, after_post->var())));
        write_back->app(true, mr.post, {w.tuple({element_final, w.tuple(post_elements)}), after_post});

        // Inner (reduction) loops over the trailing `Rr` bounds of `Sr`, collecting the reduction iteration
        // indices.
        acc              = mr.init;
        cont             = write_back;
        auto raw_red     = affine::counting_nest(current_mut, cont, acc, Defs(sr).subspan(ro, rr), "forIn");
        auto element_acc = acc;

        // The full loop iteration vector `(o…, r…)`; its moduli are exactly `Sr`.
        DefVec iters_v = out_iters;
        for (auto red : btensor::to_idxs(w, Defs(sr).subspan(ro, rr), raw_red))
            iters_v.emplace_back(red);
        auto iters = w.tuple(iters_v);

        // Read one element from each input at its affine read coordinates.
        DefVec input_elements(nis_nat, [&](size_t i) {
            auto sis_i  = mr.Sis->proj(nis_nat, i);
            auto coords = affine_map(mr.maps->proj(nis_nat, i), mr.Ris->proj(nis_nat, i), n, Sr, sis_i, iters);
            return w.extract(as_array(new_is->proj(nis_nat, i)), coords);
        });

        mr.comb->set("comb");
        mr.post->set("post");
        current_mut->app(true, mr.comb, {w.tuple({element_acc, w.tuple(input_elements)}), cont});
        return call;
    } catch (const fe::Error::Bail&) { throw; } catch (const std::exception& e) {
        fe::throwf("failed to lower `tensor.map_reduce`: {}", e.what());
    }
}

const Def* LowerMapReduce::build_pointwise(const Def* inputs,
                                           const Def* type,
                                           const Def* So,
                                           u64 ro,
                                           std::function<const Def*(Defs, const Def*)> compute) {
    auto& w = new_world();

    auto fun    = w.mut_fun(inputs->type(), type)->set("pointwise");
    auto ds_fun = cps::op_cps2ds_dep(fun)->set("dsFun");
    auto call   = w.app(ds_fun, inputs)->set("call");

    auto [new_inputs, cont] = fun->vars<2>();
    new_inputs->set("is");

    // Output loops over `So`, collecting the raw i64 iteration indices for `compute`.
    const Def* acc   = undef_tensor(type);
    auto current_mut = fun;
    auto so          = So->projs(ro);
    auto out_iters   = affine::counting_nest(current_mut, cont, acc, so, "forOut"); // raw i64 loop counters
    auto wb_matrix   = acc;

    // Write the computed element at the (identity) output coordinates; convert the i64 counters to `Idx (So#k)`.
    auto write_coords = btensor::to_idxs(w, so, out_iters);
    auto element      = compute(out_iters, new_inputs);
    current_mut->app(true, cont, as_tensor(type, w.insert(as_array(wb_matrix), w.tuple(write_coords), element)));
    return call;
}

const Def* LowerMapReduce::lower_generate(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto body = rewrite(app->arg());
    auto type = rewrite(app->type());

    auto [meta, s_out] = c->uncurry_args<2>();
    auto [T, r]        = meta->projs<2>();
    auto r_l           = Lit::isa<u64>(r);
    if (!r_l) {
        log().w("rank {} of {} is not known at lowering time", r, app);
        return nullptr;
    }
    auto rn = *r_l;

    auto unit    = w.tuple();
    auto compute = [&](Defs out_iters, const Def*) { return w.call(body, out_iters); };
    return build_pointwise(unit, type, s_out, rn, compute);
}

const Def* LowerMapReduce::lower_pad(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg()); // (input, value)
    auto type = rewrite(app->type());

    // callee: pad {T, r} [s_in] [mode, lo, hi]
    auto [Tr, s_in, params] = c->uncurry_args<3>();
    auto [T, r]             = Tr->projs<2>();
    auto [mode, lo, hi]     = params->projs<3>();

    auto r_l    = Lit::isa<u64>(r);
    auto mode_l = Lit::isa<u64>(mode);
    if (!r_l || !mode_l) {
        log().w("rank/mode of {} is not known at lowering time", app);
        return nullptr;
    }
    auto rn       = *r_l;
    auto mode_nat = *mode_l;

    auto s_out = TensorType::isa(type)->s;

    auto compute = [&](Defs out_iters, const Def* new_inputs) -> const Def* {
        auto [input, value]  = new_inputs->projs<2>();
        auto [coords, valid] = btensor::pad_coords(out_iters, lo, s_in, mode_nat != 0);
        auto elem            = w.extract(as_array(input), w.tuple(coords));
        return valid ? w.select(valid, elem, value) : elem; // constant: fill out-of-region cells with `value`
    };

    return build_pointwise(args, type, s_out, rn, compute);
}

const Def* LowerMapReduce::lower_concat(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg()); // the `is` input tuple
    auto type = rewrite(app->type());

    // callee: concat {T, nis, r} [ax] {Sis}
    auto [TnisR, ax, Sis] = c->uncurry_args<3>();
    auto [T, nis, r]      = TnisR->projs<3>();

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) {
        log().w("nis/r/ax of {} are not known at lowering time", app);
        return nullptr;
    }
    auto nisn = *nis_l, rn = *r_l, axn = *ax_l;

    auto exts = btensor::concat_extents(Sis, nisn, rn, axn);
    if (!exts) {
        log().w("an extent of {} along the concat axis is not known at lowering time", app);
        return nullptr;
    }

    // Deduce the output shape: the summed extent along `ax`, the shared extents elsewhere.
    auto sum_ax = std::accumulate(exts->begin(), exts->end(), u64(0));
    DefVec so(rn, [&](size_t d) { return d == axn ? w.lit_nat(sum_ax) : Sis->proj(nisn, 0)->proj(rn, d); });
    auto s_out = w.tuple(so);

    auto compute = [&](Defs out_iters, const Def* new_inputs) -> const Def* {
        // Read input `i` at `out_iters`, but with the `ax` coordinate replaced by `ax_idx`.
        auto read = [&](u64 i, const Def* ax_idx) -> const Def* {
            auto Sis_i = Sis->proj(nisn, i);
            DefVec coords(rn, [&](size_t d) {
                return w.call(core::conv::u, Sis_i->proj(rn, d), d == axn ? ax_idx : out_iters[d]);
            });
            return w.extract(as_array(new_inputs->proj(nisn, i)), w.tuple(coords));
        };
        return btensor::concat_select(out_iters[axn], *exts, read);
    };

    return build_pointwise(args, type, s_out, rn, compute);
}

const Def* LowerMapReduce::lower_gather(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg());
    auto type = rewrite(app->type());

    auto [Tr, shapes, dim] = c->uncurry_args<3>();
    auto [T, r]            = Tr->projs<2>();
    auto [s_src, s_idx]    = shapes->projs<2>();
    auto r_l               = Lit::isa<u64>(r);
    auto dim_l             = Lit::isa<u64>(dim);
    if (!r_l || !dim_l) return nullptr;

    // On every axis other than `dim`, gather reuses the output coordinate as
    // an input coordinate. The shared checker rejects statically provable
    // violations before any source buffer access is emitted.
    if (!check_gather_shape_constraints(app, r, dim, s_src, s_idx)) return nullptr;

    auto compute = [&](Defs out_indices, const Def* inputs) -> const Def* {
        auto element        = w.app(w.annex<tensor::gather_pointwise_elem_impl>(), {T, r});
        auto [input, index] = inputs->projs<2>();
        return w.call(element, Defs{s_src, s_idx}, dim, out_indices, Defs{as_array(input), as_array(index)});
    };
    return build_pointwise(args, type, s_idx, *r_l, compute);
}

const Def* LowerMapReduce::lower_scatter(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg());
    auto type = rewrite(app->type());

    auto [Tr, shapes, dim]         = c->uncurry_args<3>();
    auto [T, r]                    = Tr->projs<2>();
    auto [s_src, s_idx, s_updates] = shapes->projs<3>();
    auto r_l                       = Lit::isa<u64>(r);
    auto dim_l                     = Lit::isa<u64>(dim);
    if (!r_l || !dim_l) return nullptr;
    auto rn = *r_l;

    // Every scatter visit reads one update at the same coordinate as the
    // index tensor, while non-dim coordinates are also reused in the source.
    if (!check_scatter_shape_constraints(app, r, dim, s_src, s_idx, s_updates)) return nullptr;

    auto fun    = w.mut_fun(args->type(), type)->set("scatter");
    auto ds_fun = cps::op_cps2ds_dep(fun)->set("dsFun");
    auto call   = w.app(ds_fun, args)->set("call");

    auto [iiu, cont]             = fun->vars<2>();
    auto [input, index, updates] = iiu->projs<3>();
    auto acc                     = input;
    auto current                 = fun;
    auto dims                    = s_idx->projs(rn);
    auto visit_indices           = affine::counting_nest(current, cont, acc, dims, "scatter");

    auto step = w.app(w.annex<tensor::scatter_step_impl>(), {T, r});
    auto next = w.call(step, Defs{s_src, s_idx, s_updates}, dim, visit_indices,
                       Defs{as_array(acc), as_array(index), as_array(updates)});
    current->app(true, cont, as_tensor(type, next));
    return call;
}

const Def* LowerMapReduce::rewrite_imm_Nom(const Nom* nom) {
    if (auto tensor = TensorType::isa(nom))
        if (auto rank = Lit::isa<u64>(tensor->r))
            for (auto extent : tensor->s->projs(*rank))
                if (Lit::isa<u64>(extent) == 0) nom->blame("cannot lower value tensor with a zero extent").bail();
    return RWPhase::rewrite_imm_Nom(nom);
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (Axm::isa<tensor::from_array>(app) || Axm::isa<tensor::scalar>(app))
        return as_tensor(rewrite(app->type()), rewrite(app->arg()));
    if (Axm::isa<tensor::item>(app)) return as_array(rewrite(app->arg()));
    if (Axm::isa<tensor::get>(app)) {
        auto [t, i] = rewrite(app->arg())->projs<2>();
        return w.extract(as_array(t), i);
    }
    if (Axm::isa<tensor::set>(app)) {
        auto [t, i, v] = rewrite(app->arg())->projs<3>();
        return as_tensor(rewrite(app->type()), w.insert(as_array(t), i, v));
    }
    if (Axm::isa<tensor::splat>(app)) {
        auto type  = rewrite(app->type());
        auto shape = TensorType::isa(type)->s;
        return as_tensor(type, w.pack(shape, rewrite(app->arg()->proj(2, 1))));
    }

    // A `tensor.if_static` still stuck at lowering time guards a runtime value: residualize to
    // its dynamic branch.
    if (Axm::isa<tensor::if_static>(app)) return rewrite(app->arg(3, 2));
    if (auto bc = Axm::isa<tensor::broadcast>(app)) {
        if (auto res = lower_broadcast(bc)) return res;
    } else if (auto mr = Axm::isa<tensor::map_reduce_post>(app)) {
        if (auto res = lower_map_reduce(mr)) return res;
    } else if (auto generate = Axm::isa<tensor::generate>(app)) {
        if (auto res = lower_generate(generate)) return res;
    } else if (auto pad = Axm::isa<tensor::pad>(app)) {
        if (auto res = lower_pad(pad)) return res;
    } else if (auto cat = Axm::isa<tensor::concat>(app)) {
        if (auto res = lower_concat(cat)) return res;
    } else if (auto gather = Axm::isa<tensor::gather>(app)) {
        if (auto res = lower_gather(gather)) return res;
    } else if (auto scatter = Axm::isa<tensor::scatter>(app)) {
        if (auto res = lower_scatter(scatter)) return res;
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
