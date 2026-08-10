#include "mim/plug/tensor/phase/fuse.h"

#include <algorithm>
#include <optional>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/util/types.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

/// If `e` reads coordinate `var#i` injectively, returns `i`:
/// a plain extract, possibly strided (`%affine.semiop.mul` by a non-zero literal) and/or shifted
/// (`%affine.op.add`/`sub` with a loop-invariant `%affine.constant` on the other side).
/// Everything else — `mod`/`div`, sums of two loop indices (convolution windows), symbolic strides
/// (which may be 0 at runtime) — yields nothing.
static std::optional<u64> injective_coord(const Def* var, const Def* e) {
    // A one-loop domain «1; %affine.index» collapses to a plain %affine.index, so the var itself is coordinate 0.
    if (e == var) return 0;
    if (auto ex = e->isa<Extract>(); ex && ex->tuple() == var) return Lit::isa<u64>(ex->index());

    if (auto semiop = Axm::isa(affine::semiop::mul, e)) {
        auto [x, c] = semiop->args<2>();
        if (auto lit = Lit::isa<u64>(c); lit && *lit != 0) return injective_coord(var, x);
        return {};
    }

    if (auto op = Axm::isa<affine::op>(e)) {
        if (op.id() != affine::op::add && op.id() != affine::op::sub) return {};
        auto [a, b]  = op->args<2>();
        bool a_const = static_cast<bool>(Axm::isa<affine::constant>(a));
        bool b_const = static_cast<bool>(Axm::isa<affine::constant>(b));
        if (a_const == b_const) return {};
        return injective_coord(var, a_const ? b : a);
    }

    return {};
}

/// Checks that `map` provably reads through *every* loop index of its domain: its body is the
/// identity, or each result coordinate is an injective read of one loop index (see `injective_coord`)
/// and together they cover all indices.
/// Such a map is injective over the iteration domain, so each element of the input behind it is read
/// (and, after fusion, computed) *at most* once — strided/shifted reads skip elements entirely, so
/// fusing behind them even drops computations the consumer never looks at.
/// Anything not provably injective — a dropped loop index, a wrapped coordinate (`mod`, ...), a
/// non-literal rank — is conservatively rejected.
static bool reads_injectively(const Def* map) {
    auto lam = map->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return false;
    auto var  = lam->var();
    auto body = lam->body();
    if (body == var) return true; // identity: every loop index passes through

    auto n = Lit::isa<u64>(var->type()->arity());
    if (!n) return false;

    Vector<bool> used(*n, false);
    auto mark = [&](const Def* elem) {
        auto i = injective_coord(var, elem);
        if (!i || *i >= *n) return false;
        used[*i] = true;
        return true;
    };

    if (auto tuple = body->isa<Tuple>()) {
        for (auto elem : tuple->ops())
            if (!mark(elem)) return false;
    } else if (auto pack = body->isa<Pack>()) {
        // A pack repeats a single coordinate, so it can only cover a one-loop domain.
        if (!pack->is_set() || !mark(pack->body())) return false;
    } else if (!mark(body)) {
        return false;
    }

    return std::ranges::all_of(used, std::identity{});
}

/// Is `post` the (rebuilt) CPS identity `%tensor.id`, i.e. a lam that just returns its argument?
/// Recognized structurally, since the rewrite into this phase's world breaks pointer equality.
static bool is_identity_post(const Def* post) {
    auto lam = post->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return false;
    auto app = lam->body()->isa<App>();
    return app && app->callee() == lam->var(1) && app->arg() == lam->var(0);
}

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
// map (`Sr = So`, `map_out = %affine.id`), *and* the outer reads that input injectively (its
// access map uses every loop index, see `reads_injectively`). Reading such an inner tensor at a
// position is then just a single call to the inner combination function, with each inner access
// map composed behind the outer's access map for that input; injectivity guarantees that this
// inlining performs each inner computation at most once, so fusion never duplicates work.
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

    auto [To, Tp, Ro, Rr]   = meta->projs<4>();
    auto [comb, init, post] = comb_init->projs<3>();
    auto [Tis, Ris, Sis]    = TisRisSis->projs<3>();
    auto is                 = rewrite(app->arg());

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
        auto inner   = Axm::isa<tensor::map_reduce_post>(input_k);
        if (!inner) continue;

        auto [inner_nis, inner_meta, inner_shapes, inner_TisRisSis, inner_comb_init, inner_map_out, inner_maps,
              inner_is]                               = inner->uncurry_args<8>();
        auto [inner_To, inner_Tp, inner_Ro, inner_Rr] = inner_meta->projs<4>();
        auto [inner_So, inner_Sr]                     = inner_shapes->projs<2>();
        auto [inner_comb, inner_init, inner_post]     = inner_comb_init->projs<3>();
        auto [inner_Tis, inner_Ris, inner_Sis]        = inner_TisRisSis->projs<3>();

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

        // A non-identity inner epilogue cannot be dropped when the inner combiner is inlined;
        // threading it through the fused combiner chain is future work.
        if (!is_identity_post(inner_post)) continue;

        // Fusing inlines the inner combiner once per iteration of the outer's *full* loop nest.
        // Unless the outer reads input k injectively, the same inner element is recomputed once
        // for every iteration of each loop its access map ignores — e.g. a matrix product reads
        // its first input at `(i, k)`, so a fused producer would be recomputed for every `j`.
        // In that case keep the producer materialized instead.
        if (!reads_injectively(maps->proj(nis_nat, k))) continue;

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

    // Construct the fused map_reduce; the loop domain, output map, init and epilogue are the outer's.
    auto mr = w.annex<tensor::map_reduce_post>();
    mr      = w.app(mr, new_nis_def);
    mr      = w.app(mr, meta);
    mr      = w.app(mr, shapes);
    mr      = w.app(mr, {new_Tis, new_Ris, new_Sis});
    mr      = w.app(mr, {new_comb, init, post});
    mr      = w.app(mr, map_out);
    mr      = w.app(mr, new_maps);
    mr      = w.app(mr, new_is);

    return mr;
}

// Fuses a trailing pure map into the reducing `tensor.map_reduce` producing its input — the reverse
// of `fuse_map_reduce` and the direction the producer gate deliberately rejects for `Rr > 0`: a
// reduction must not be inlined into a consumer's combiner (it would rerun per fold step), but it
// *can* absorb the consumer into its per-output-cell `post` epilogue (the GEMM-epilogue pattern,
// e.g. `unary(prod2d)`).
//
// Outer:    map_reduce nis=1 (To, Tp, Ro, 0) (So, So) (f_o, init_o, post_o) id (id,) inner
// Inner:    map_reduce nis_i (To_i, Tp_i, Ro_i, Rr_i) (So_i, Sr_i) (f_i, init_i, post_i) map_out_i maps_i is_i
// Result:   the inner op with post := post_o ∘ f_o(init_o, ·) ∘ post_i and out-element type Tp.
//
// v1 restrictions: the outer must be a single-input map over its full domain (`Rr = 0`, `Sr = So`,
// identity `map_out`) reading that input elementwise (identity access map covering the whole input,
// `Sis#0 = So`) — anything else changes coordinates or drops cells and would need map inversion.
// The inner must be consumed by this outer alone (checked on the old world): otherwise it stays
// materialized for the other consumers and additionally runs fused — duplicating the whole
// reduction loop nest.
const Def* Fuse::fuse_epilogue(const App* app) {
    auto outer_callee = rewrite(app->callee())->as<App>();

    auto [nis, meta, shapes, TisRisSis, comb_init, map_out, maps] = outer_callee->uncurry_args<7>();

    auto [To, Tp, Ro, Rr]   = meta->projs<4>();
    auto [So, Sr]           = shapes->projs<2>();
    auto [comb, init, post] = comb_init->projs<3>();
    auto [Tis, Ris, Sis]    = TisRisSis->projs<3>();

    auto& w = new_world();

    auto nis_lit = Lit::isa<u64>(nis);
    if (!nis_lit || *nis_lit != 1) return nullptr;
    auto rr_lit = Lit::isa<u64>(Rr);
    if (!rr_lit || *rr_lit != 0) return nullptr;
    if (Sr != So) return nullptr;
    if (Sis->proj(1, 0) != So) return nullptr; // the map must cover its input exactly

    auto id_out = map_out->isa_mut<Lam>();
    if (!id_out || !id_out->is_set() || id_out->body() != id_out->var()) return nullptr;
    auto id_in = maps->proj(1, 0)->isa_mut<Lam>();
    if (!id_in || !id_in->is_set() || id_in->body() != id_in->var()) return nullptr;

    auto inner = Axm::isa<tensor::map_reduce_post>(rewrite(app->arg())->proj(1, 0));
    if (!inner) return nullptr;

    // Single-consumer guard on the old world: the inner op — and its wrapping argument tuple, if
    // any — must be used by this outer app alone.
    auto old_input = app->arg()->proj(1, 0);
    if (auto it = num_users_.find(old_input); it == num_users_.end() || it->second != 1) return nullptr;
    if (old_input != app->arg()) {
        if (auto it = num_users_.find(app->arg()); it == num_users_.end() || it->second != 1) return nullptr;
    }

    auto [inner_nis, inner_meta, inner_shapes, inner_TisRisSis, inner_comb_init, inner_map_out, inner_maps, inner_is]
        = inner->uncurry_args<8>();
    auto [inner_To, inner_Tp, inner_Ro, inner_Rr] = inner_meta->projs<4>();
    auto [inner_comb, inner_init, inner_post]     = inner_comb_init->projs<3>();

    DLOG("fusing trailing map {} into the epilogue of {}", app, inner);

    // The composed epilogue `post_o ∘ f_o(init_o, ·) ∘ post_i`, as a CPS chain. Identity hops are
    // called through anyway — their always-true filters inline them.
    auto fused_post = w.mut_con({inner_To, w.cn(Tp)})->set("fused_post");
    auto after_ip   = w.mut_con(inner_Tp)->set("afterInnerPost");
    auto after_comb = w.mut_con(To)->set("afterComb");
    fused_post->app(true, inner_post, {fused_post->var(0), after_ip});
    after_ip->app(true, comb, {w.tuple({init, w.tuple({after_ip->var(0)})}), after_comb});
    after_comb->app(true, post, {after_comb->var(0), fused_post->var(1)});

    // The inner op, keeping its loop nest untouched; only the epilogue and the out-element type change.
    auto mr = w.annex<tensor::map_reduce_post>();
    mr      = w.app(mr, inner_nis);
    mr      = w.app(mr, {inner_To, Tp, inner_Ro, inner_Rr});
    mr      = w.app(mr, inner_shapes);
    mr      = w.app(mr, inner_TisRisSis);
    mr      = w.app(mr, {inner_comb, inner_init, fused_post});
    mr      = w.app(mr, inner_map_out);
    mr      = w.app(mr, inner_maps);
    mr      = w.app(mr, inner_is);

    return mr;
}

/// Does `def`'s user count matter for the epilogue direction's single-consumer guard?
/// `fuse_epilogue` only ever queries `map_reduce_post` apps (the fusion candidates) and defs
/// carrying one as an immediate operand (their wrapping argument tuples) — everything else
/// need not be counted.
static bool counts_users(const Def* def) {
    if (Axm::isa<tensor::map_reduce_post>(def)) return true;
    for (auto op : def->ops())
        if (op && Axm::isa<tensor::map_reduce_post>(op)) return true;
    return false;
}

void Fuse::start() {
    // The epilogue direction needs to know whether a map_reduce is consumed by a single user; the
    // old world does not track uses, so count users in one pass up front.
    unique_queue<DefSet> wl;
    for (auto root : old_world().roots())
        wl.push(root);
    while (!wl.empty()) {
        auto def = wl.pop();
        for (auto op : def->ops()) {
            if (!op) continue;
            if (counts_users(op)) ++num_users_[op];
            wl.push(op);
        }
        if (def->type()) wl.push(def->type());
    }
    RWPhase::start();
}

const Def* Fuse::rewrite_imm_App(const App* app) {
    if (auto mr = Axm::isa<tensor::map_reduce_post>(app)) {
        if (auto res = fuse_map_reduce(mr)) {
            DLOG("Fused map_reduce at {} into a new map_reduce {}", app, res);
            return res;
        }
        if (auto res = fuse_epilogue(mr)) {
            DLOG("Fused the trailing map at {} into its producer's epilogue: {}", app, res);
            return res;
        }
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
