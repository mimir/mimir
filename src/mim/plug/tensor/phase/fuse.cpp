#include "mim/plug/tensor/phase/fuse.h"

#include "mim/def.h"
#include "mim/lam.h"

#include "mim/util/types.h"

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

/// `inner ∘ outer`: feeds the outer op's read coordinates for one input into the inner op's access map.
static const Def* compose_map(World& w, const Def* inner, const Def* outer) {
    auto dom   = outer->type()->as<Pi>()->dom();
    auto codom = inner->type()->as<Pi>()->codom();
    auto lam   = w.mut_lam(dom, codom)->set("fused_map");
    lam->set(true, w.app(inner, w.app(outer, lam->var())));
    return lam;
}

// Fuses an outer `tensor.map_reduce` with any number of its inputs — and, recursively, any
// fusible inputs of those inputs — whenever each such input is itself a `tensor.map_reduce`
// without reduction loops (`Rr = 0`) that writes its full loop domain through the identity output
// map (`Sr = So`, `map_out = %affine.id`). Reading such an inner tensor at a position is then just
// a single call to the inner combination function, with each inner access map composed behind the
// outer's access map for that input.
//
// Outer:    map_reduce nis_o (To, Ro, Rr) (So, Sr) (Tis_o, Ris_o, Sis_o) (f_o, init_o) map_out maps_o is_o
// Inner:    map_reduce nis_k (To_k, Ro_k, 0) (So_k, So_k) (Tis_k, Ris_k, Sis_k) (f_k, init_k) id maps_k is_k
//           for every fusible input — possibly nested inside another fusible input
//
// Result:   map_reduce nis_new (To, Ro, Rr) (So, Sr) (Tis_new, Ris_new, Sis_new) (f_new, init_o)
//           map_out maps_new is_new
//
// The collection phase walks the tree of fusible inner ops below `app` once, producing a flat list
// of *leaves* (the surviving tensor inputs of the fused op) and *inner nodes* (the inner combiners
// that must run before `f_o`). Each fusible input is replaced by its inner's inputs, with access
// maps composed behind the outer map at that position; the composition nests across levels. The
// new combination function `f_new` invokes every inner combiner in post-order — each starting from
// its own init — and finally invokes `f_o`, threading inner results into the corresponding outer
// input slots.
const Def* Fuse::fuse_map_reduce(const App* app) {
    auto outer_callee = rewrite(app->callee())->as<App>();

    auto [nis, meta, shapes, TisRisSis, comb_init, map_out, maps] = outer_callee->uncurry_args<7>();

    auto [To, Ro, Rr]    = meta->projs<3>();
    auto [comb, init]    = comb_init->projs<2>();
    auto [Tis, Ris, Sis] = TisRisSis->projs<3>();
    auto is              = rewrite(app->arg());

    DLOG("considering map_reduce for fusion:");
    DLOG("  comb = {} : {}", comb, comb->type());
    DLOG("  init = {} : {}", init, init->type());
    DLOG("  Tis = {} : {}", Tis, Tis->type());
    DLOG("  Ris = {} : {}", Ris, Ris->type());
    DLOG("  Sis = {} : {}", Sis, Sis->type());
    DLOG("  To = {} : {}", To, To->type());
    DLOG("  Ro = {} : {}", Ro, Ro->type());
    DLOG("  nis = {} : {}", nis, nis->type());
    DLOG("  is = {} : {}", is, is->type());

    auto& w = new_world();

    auto nis_lit = Lit::isa<u64>(nis);
    if (!nis_lit) return nullptr;
    auto nis_nat = *nis_lit;

    struct InnerInfo {
        bool fusible    = false;
        const Def* comb = nullptr;
        const Def* init = nullptr;
        const Def* Tis  = nullptr;
        const Def* Ris  = nullptr;
        const Def* Sis  = nullptr;
        const Def* To   = nullptr;
        const Def* maps = nullptr;
        u64 nis         = 0;
        const Def* is   = nullptr;
    };

    Vector<InnerInfo> infos(nis_nat);
    bool any_fusible = false;

    for (u64 k = 0; k < nis_nat; ++k) {
        auto input_k = is->proj(nis_nat, k);
        auto inner   = Axm::isa<tensor::map_reduce>(input_k);
        if (!inner) continue;

        auto [inner_nis, inner_meta, inner_shapes, inner_TisRisSis, inner_comb_init, inner_map_out, inner_maps,
              inner_is]                        = inner->uncurry_args<8>();
        auto [inner_To, inner_Ro, inner_Rr]    = inner_meta->projs<3>();
        auto [inner_So, inner_Sr]              = inner_shapes->projs<2>();
        auto [inner_comb, inner_init]          = inner_comb_init->projs<2>();
        auto [inner_Tis, inner_Ris, inner_Sis] = inner_TisRisSis->projs<3>();

        auto inner_nis_nat = Lit::isa<u64>(inner_nis);
        if (!inner_nis_nat) continue;

        // We can only fuse when the inner has no reduction loops and writes every cell of its full
        // loop domain through the identity output map. In that case the inner tensor at any
        // position is just a single call of `inner_comb` at that position.
        // The identity map (`%affine.id`) is recognized structurally (a lam returning its own var),
        // since the rewrite into this phase's world rebuilds mutables and breaks pointer equality.
        auto inner_rr = Lit::isa<u64>(inner_Rr);
        if (!inner_rr || *inner_rr != 0) continue;
        if (inner_Sr != inner_So) continue;
        auto id_lam = inner_map_out->isa_mut<Lam>();
        if (!id_lam || !id_lam->is_set() || id_lam->body() != id_lam->var()) continue;

        auto& info   = infos[k];
        info.fusible = true;
        info.comb    = inner_comb;
        info.init    = inner_init;
        info.Tis     = inner_Tis;
        info.Ris     = inner_Ris;
        info.Sis     = inner_Sis;
        info.To      = inner_To;
        info.maps    = inner_maps;
        info.nis     = *inner_nis_nat;
        info.is      = inner_is;
        any_fusible  = true;
    }

    if (!any_fusible) return nullptr;

    // Each fusible outer input k is replaced by `infos[k].nis` slots in the fused input list;
    // every non-fusible input retains exactly one slot. `new_pos[i]` is the start of input i's
    // slot range in the fused list.
    Vector<u64> new_pos(nis_nat);
    u64 new_nis_nat = 0;
    for (u64 i = 0; i < nis_nat; ++i) {
        new_pos[i] = new_nis_nat;
        new_nis_nat += infos[i].fusible ? infos[i].nis : 1;
    }

    DefVec new_Tis_vec(new_nis_nat);
    DefVec new_Ris_vec(new_nis_nat);
    DefVec new_Sis_vec(new_nis_nat);
    DefVec new_maps_vec(new_nis_nat);
    DefVec new_is_vec(new_nis_nat);

    for (u64 i = 0; i < nis_nat; ++i) {
        if (infos[i].fusible) {
            const auto& info = infos[i];
            auto outer_map_i = maps->proj(nis_nat, i);
            for (u64 l = 0; l < info.nis; ++l) {
                auto pos         = new_pos[i] + l;
                new_Tis_vec[pos] = info.Tis->proj(info.nis, l);
                new_Ris_vec[pos] = info.Ris->proj(info.nis, l);
                new_Sis_vec[pos] = info.Sis->proj(info.nis, l);
                new_is_vec[pos]  = info.is->proj(info.nis, l);
                // The inner reads at its own output coordinates; those are the outer's read
                // coordinates for input i, so the fused access map is the composition.
                new_maps_vec[pos] = compose_map(w, info.maps->proj(info.nis, l), outer_map_i);
            }
        } else {
            auto pos          = new_pos[i];
            new_Tis_vec[pos]  = Tis->proj(nis_nat, i);
            new_Ris_vec[pos]  = Ris->proj(nis_nat, i);
            new_Sis_vec[pos]  = Sis->proj(nis_nat, i);
            new_maps_vec[pos] = maps->proj(nis_nat, i);
            new_is_vec[pos]   = is->proj(nis_nat, i);
        }
    }

    auto new_Tis  = w.tuple(new_Tis_vec);
    auto new_Ris  = w.tuple(new_Ris_vec);
    auto new_Sis  = w.tuple(new_Sis_vec);
    auto new_maps = w.tuple(new_maps_vec);
    auto new_is   = w.tuple(new_is_vec);

    auto new_nis_def = w.lit_nat(new_nis_nat);

    // Build the fused combination function:
    //
    //   cn f_new(data: [To, [new_Tis ...]], ret: cn To) =
    //       cn inner_ret_<r>(value_<r>: inner_To_<r>) = ...
    //       f_<fused[0]>((init_<fused[0]>, inner_inputs_<fused[0]>), inner_ret_0)
    //
    //   inner_ret_<r>(value_<r>):
    //       if r is not the last fused input:
    //           f_<fused[r+1]>((init_<fused[r+1]>, inner_inputs_<fused[r+1]>), inner_ret_<r+1>)
    //       else:
    //           f_o((acc, outer_inputs), ret)
    //
    // `outer_inputs[i]` is `value_<r>` when input i is the r-th fused input, and the
    // corresponding `new_in` slot otherwise. Each `inner_ret_<r>` closes over the prior
    // `value_<j>`s as free variables — those are bound by the dynamic call chain.
    auto inputs_sigma = w.sigma(new_Tis_vec);
    auto data_sigma   = w.sigma({To, inputs_sigma});
    auto ret_cn_type  = w.cn(To);
    auto new_comb     = w.mut_con({data_sigma, ret_cn_type})->set("fused_comb");
    auto new_data     = new_comb->var(0);
    auto new_ret      = new_comb->var(1);
    auto new_acc      = new_data->proj(2, 0);
    auto new_in       = new_data->proj(2, 1);

    Vector<u64> fused_indices;
    for (u64 i = 0; i < nis_nat; ++i)
        if (infos[i].fusible) fused_indices.emplace_back(i);

    Vector<Lam*> inner_rets(fused_indices.size());
    Vector<const Def*> inner_values(fused_indices.size());
    for (size_t r = 0; r < fused_indices.size(); ++r) {
        auto new_inner_To = infos[fused_indices[r]].To;
        inner_rets[r]     = w.mut_con(new_inner_To)->set("inner_ret");
        inner_values[r]   = inner_rets[r]->var(0);
    }

    // Map each outer input position to its value at the f_o call site.
    DefVec outer_inputs_vec(nis_nat);
    {
        size_t r = 0;
        for (u64 i = 0; i < nis_nat; ++i)
            if (infos[i].fusible)
                outer_inputs_vec[i] = inner_values[r++];
            else
                outer_inputs_vec[i] = new_in->proj(new_nis_nat, new_pos[i]);
    }

    // Chain: caller for fused step r is new_comb (r==0) or inner_rets[r-1] (otherwise).
    for (size_t r = 0; r < fused_indices.size(); ++r) {
        auto k              = fused_indices[r];
        auto new_inner_comb = infos[k].comb;
        auto new_inner_init = infos[k].init;

        DefVec inner_inputs_vec(infos[k].nis);
        for (u64 l = 0; l < infos[k].nis; ++l)
            inner_inputs_vec[l] = new_in->proj(new_nis_nat, new_pos[k] + l);

        Lam* caller = (r == 0) ? new_comb : inner_rets[r - 1];
        caller->app(true, new_inner_comb, {w.tuple({new_inner_init, w.tuple(inner_inputs_vec)}), inner_rets[r]});
    }

    // After every inner combiner has produced its value, call the outer combiner.
    inner_rets.back()->app(true, comb, {w.tuple({new_acc, w.tuple(outer_inputs_vec)}), new_ret});

    // Construct the fused map_reduce; the loop domain, output map and init are the outer's.
    auto mr = w.annex<tensor::map_reduce>();
    mr      = w.app(mr, new_nis_def);
    mr      = w.app(mr, meta);
    mr      = w.app(mr, shapes);
    mr      = w.app(mr, {new_Tis, new_Ris, new_Sis});
    mr      = w.app(mr, {new_comb, init});
    mr      = w.app(mr, map_out);
    mr      = w.app(mr, new_maps);
    mr      = w.app(mr, new_is);

    return mr;
}

const Def* Fuse::rewrite_imm_App(const App* app) {
    if (auto mr = Axm::isa<tensor::map_reduce>(app)) {
        if (auto res = fuse_map_reduce(mr)) {
            DLOG("Fused map_reduce at {} into a new map_reduce {}", app, res);
            return res;
        }
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
