#include "mim/plug/tensor/phase/fuse.h"

#include <optional>

#include <fe/bitset.h>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/util/types.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/btensor/btensor.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

/// If `e` reads coordinate `var#i` injectively, returns `i`:
/// a plain extract, possibly strided (`affine.semiop.mul` by a non-zero literal) and/or shifted
/// (`affine.op.add`/`sub` with a loop-invariant `affine.lit` on the other side).
/// Everything else — `mod`/`div`, sums of two loop indices (convolution windows), symbolic strides
/// (which may be 0 at runtime) — yields nothing.
static std::optional<u64> injective_coord(const Def* var, const Def* e) {
    // A one-loop domain «1; affine.Index» collapses to a plain affine.Index, so the var itself is coordinate 0.
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
        bool a_const = static_cast<bool>(Axm::isa<affine::lit>(a));
        bool b_const = static_cast<bool>(Axm::isa<affine::lit>(b));
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

    auto used = fe::Bitset();
    auto mark = [&](const Def* elem) {
        auto i = injective_coord(var, elem);
        if (!i || *i >= *n) return false;
        used.set(*i);
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

    return used.count() == *n;
}

/// `inner ∘ outer`: feeds the outer op's read coordinates for one input into the inner op's access map.
static const Def* compose_map(World& w, const Def* inner, const Def* outer) {
    auto dom   = outer->type()->as<Pi>()->dom();
    auto codom = inner->type()->as<Pi>()->codom();
    auto lam   = w.mut_lam(dom, codom)->set("fused_map");
    lam->set(true, w.app(inner, w.app(outer, lam->var())));
    return lam;
}

/// The five parallel per-slot lists of a `map_reduce_post` input group: element type, rank, shape,
/// access map, and the tensor itself.
struct Slots {
    DefVec T, R, S, maps, is;

    void push(const Def* t, const Def* r, const Def* s, const Def* m, const Def* v) {
        T.emplace_back(t), R.emplace_back(r), S.emplace_back(s), maps.emplace_back(m), is.emplace_back(v);
    }
};

/// Does a slot that declares @p value in shape @p S read it in the shape it is produced in?
/// Size-1 axes collapse out of the type, so they may differ - and then their maps do not compose.
static bool reads_as_produced(const Def* value, const Def* S) {
    if (auto mr = Axm::isa<tensor::map_reduce_post>(value)) {
        auto shapes = mr->uncurry_args<8>()[2];
        return shapes->proj(3, 0) == S;
    }
    if (auto bc = Axm::isa<tensor::broadcast>(value)) return bc->arg()->proj(3, 1) == S;
    return true;
}

/// If `value` is a pure re-indexed read — a copy-combiner map_reduce (reshape/transpose/slice/
/// flip/repeat lower to these) or a `tensor.broadcast` — returns its source and access map, to be
/// composed behind the consuming slot's map. Such reads perform no computation, so reading through
/// them needs neither an injectivity gate nor a consumer count.
static std::optional<PureRead> read_through(World& w, const Def* value, const Def* slot_map, const Def* S) {
    if (!reads_as_produced(value, S)) return {};
    if (auto pr = is_pure_read(value)) {
        if (!TensorType::isa(pr->src->type())) return {};
        return pr;
    }

    if (auto bc = Axm::isa<tensor::broadcast>(value)) {
        auto [T, r]               = bc->callee()->as<App>()->args<2>();
        auto [s_in, s_out, input] = bc->arg()->projs<3>();
        auto r_l                  = Lit::isa<u64>(r);
        if (!r_l) return {};
        if (!TensorType::isa(input->type())) return {};

        // Source axis d reads o#d where the sizes agree and index 0 where the source axis is 1
        // (expressed as `o#d · 0`, like `bid_map`). Bail on axes where neither is provable.
        auto vec_ty = slot_map->type()->as<Pi>()->codom(); // «r; affine.Index»
        auto lam    = w.mut_lam(vec_ty, vec_ty)->set("bcast_map");
        DefVec elems(*r_l);
        for (u64 d = 0; d < *r_l; ++d) {
            auto in_d = s_in->proj(*r_l, d);
            auto o_d  = lam->var(*r_l, d);
            if (in_d == s_out->proj(*r_l, d))
                elems[d] = o_d;
            else if (auto l = Lit::isa<u64>(in_d); l && *l == 1)
                elems[d] = w.call(affine::semiop::mul, Defs{o_d, w.lit_nat(0)});
            else
                return {};
        }
        lam->set(true, w.tuple(elems));

        return PureRead{input, lam, T, r, s_in};
    }

    return {};
}

// Fuses an outer `tensor.map_reduce` with any number of its inputs — and, recursively, any
// fusible inputs of those inputs — whenever each such input is itself a `tensor.map_reduce`
// without reduction loops (`Rr = 0`) that writes its full loop domain through the identity output
// map (`Sr = So`, `map_out = affine.id`), *and* the outer reads that input injectively (its
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
    auto mr            = btensor::MapRedPost::of(rewrite(app->callee())->as<App>());
    auto [is, post_is] = rewrite(app->arg())->projs<2>();

    log().d("consider for fusion: comb = {}, init = {}, To = {}, Ro = {}", mr.comb, mr.init, mr.To, mr.Ro);
    log().d("    inputs: nis = {}, Tis = {}, Ris = {}, Sis = {}, is = {}", mr.nis, mr.Tis, mr.Ris, mr.Sis, is);

    auto& w = new_world();

    auto nis_lit = Lit::isa<u64>(mr.nis);
    if (!nis_lit) return nullptr;
    auto nis_nat = *nis_lit;

    struct InnerInfo {
        const Def* comb;
        const Def* init;
        const Def* Tis;
        const Def* Ris;
        const Def* Sis;
        const Def* To;
        const Def* maps;
        u64 nis;
        const Def* is;
    };

    fe::Vector<std::optional<InnerInfo>> infos(nis_nat);

    for (u64 k = 0; k < nis_nat; ++k) {
        auto inner = Axm::isa<tensor::map_reduce_post>(is->proj(nis_nat, k));
        if (!inner) continue;

        auto im       = btensor::MapRedPost::of(inner->callee()->as<App>());
        auto inner_is = inner->arg()->proj(2, 0);

        auto inner_nis_nat = Lit::isa<u64>(im.nis);
        if (!inner_nis_nat || im.So != mr.Sis->proj(nis_nat, k)) continue; // see reads_as_produced

        // Epilogue inputs on the inner would have to be re-read inside the fused combiner; only
        // epilogue-free inners fuse (their identity post is checked below).
        auto inner_nps_nat = Lit::isa<u64>(im.nps);
        if (!inner_nps_nat || *inner_nps_nat != 0) continue;

        // We can only fuse when the inner has no reduction loops and writes every cell of its full
        // loop domain through the identity output map. In that case the inner tensor at any
        // position is just a single call of `inner_comb` at that position.
        // The identity map (`affine.id`) is recognized structurally (a lam returning its own var),
        // since the rewrite into this phase's world rebuilds mutables and breaks pointer equality.
        auto inner_ro = Lit::isa<u64>(im.Ro);
        auto inner_rn = Lit::isa<u64>(im.Rn);
        if (!inner_ro || !inner_rn || *inner_ro != *inner_rn) continue;
        if (im.Sr != im.So) continue;
        if (!is_identity_map(im.map_out)) continue;

        // A non-identity inner epilogue cannot be dropped when the inner combiner is inlined;
        // threading it through the fused combiner chain is future work.
        if (!is_identity_post(im.post)) continue;

        // Fusing inlines the inner combiner once per iteration of the outer's *full* loop nest.
        // Unless the outer reads input k injectively, the same inner element is recomputed once
        // for every iteration of each loop its access map ignores — e.g. a matrix product reads
        // its first input at `(i, k)`, so a fused producer would be recomputed for every `j`.
        // In that case keep the producer materialized instead.
        if (!reads_injectively(mr.maps->proj(nis_nat, k))) continue;

        infos[k] = InnerInfo{.comb = im.comb,
                             .init = im.init,
                             .Tis  = im.Tis,
                             .Ris  = im.Ris,
                             .Sis  = im.Sis,
                             .To   = im.To,
                             .maps = im.maps,
                             .nis  = *inner_nis_nat,
                             .is   = inner_is};
    }

    if (std::ranges::none_of(infos, [](const auto& info) { return info.has_value(); })) return nullptr;

    // Each fusible outer input k is replaced by `infos[k].nis` slots in the fused input list;
    // every non-fusible input retains exactly one slot. `new_pos[i]` is the start of input i's
    // slot range in the fused list.
    fe::Vector<u64> new_pos(nis_nat);
    u64 new_nis_nat = 0;
    for (u64 i = 0; i < nis_nat; ++i) {
        new_pos[i] = new_nis_nat;
        new_nis_nat += infos[i] ? infos[i]->nis : 1;
    }

    auto slots = Slots();
    for (u64 i = 0; i < nis_nat; ++i) {
        if (auto& info = infos[i]) {
            auto outer_map_i = mr.maps->proj(nis_nat, i);
            // Inlining a shared inner forwards its consumers to its inputs: they are no longer
            // sole-consumed for the epilogue guard.
            bool shared = !sole_consumer(is->proj(nis_nat, i));
            for (u64 l = 0; l < info->nis; ++l) {
                auto v = info->is->proj(info->nis, l);
                if (shared) shared_.insert(v);
                // The inner reads at its own output coordinates; those are the outer's read
                // coordinates for input i, so the fused access map is the composition.
                slots.push(info->Tis->proj(info->nis, l), info->Ris->proj(info->nis, l), info->Sis->proj(info->nis, l),
                           compose_map(w, info->maps->proj(info->nis, l), outer_map_i), v);
            }
        } else {
            slots.push(mr.Tis->proj(nis_nat, i), mr.Ris->proj(nis_nat, i), mr.Sis->proj(nis_nat, i),
                       mr.maps->proj(nis_nat, i), is->proj(nis_nat, i));
        }
    }

    auto new_Tis  = w.tuple(slots.T);
    auto new_Ris  = w.tuple(slots.R);
    auto new_Sis  = w.tuple(slots.S);
    auto new_maps = w.tuple(slots.maps);
    auto new_is   = w.tuple(slots.is);

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
    auto inputs_sigma        = w.sigma(slots.T);
    auto data_sigma          = w.sigma({mr.To, inputs_sigma});
    auto ret_cn_type         = w.cn(mr.To);
    auto new_comb            = w.mut_con({data_sigma, ret_cn_type})->set("fused_comb");
    auto [new_data, new_ret] = new_comb->vars<2>();
    auto [new_acc, new_in]   = new_data->projs<2>();

    fe::Vector<u64> fused_indices;
    for (u64 i = 0; i < nis_nat; ++i)
        if (infos[i]) fused_indices.emplace_back(i);

    fe::Vector<Lam*> inner_rets(fused_indices.size(),
                                [&](size_t r) { return w.mut_con(infos[fused_indices[r]]->To)->set("inner_ret"); });

    // Map each outer input position to its value at the f_o call site.
    DefVec outer_inputs_vec(nis_nat);
    for (u64 i = 0, r = 0; i < nis_nat; ++i)
        outer_inputs_vec[i] = infos[i] ? inner_rets[r++]->var() : new_in->proj(new_nis_nat, new_pos[i]);

    // Chain: caller for fused step r is new_comb (r==0) or inner_rets[r-1] (otherwise).
    for (size_t r = 0; r < fused_indices.size(); ++r) {
        const auto& info = *infos[fused_indices[r]];
        DefVec inner_inputs_vec(info.nis,
                                [&](size_t l) { return new_in->proj(new_nis_nat, new_pos[fused_indices[r]] + l); });
        Lam* caller = r == 0 ? new_comb : inner_rets[r - 1];
        caller->app(true, info.comb, {w.tuple({info.init, w.tuple(inner_inputs_vec)}), inner_rets[r]});
    }

    // After every inner combiner has produced its value, call the outer combiner.
    inner_rets.back()->app(true, mr.comb, {w.tuple({new_acc, w.tuple(outer_inputs_vec)}), new_ret});

    // Construct the fused map_reduce; the loop domain, output map, init and epilogue (including the
    // epilogue inputs) are the outer's.
    auto res = w.annex<tensor::map_reduce_post>();
    res      = w.app(res, {new_nis_def, mr.nps});
    res      = w.app(res, mr.meta);
    res      = w.app(res, mr.shapes);
    res      = w.app(res, {new_Tis, new_Ris, new_Sis, mr.Tps, mr.Rps, mr.Sps});
    res      = w.app(res, {new_comb, mr.init, mr.post});
    res      = w.app(res, mr.map_out);
    res      = w.app(res, {new_maps, mr.post_maps});
    res      = w.app(res, {new_is, post_is});

    return res;
}

/// Is `map` the row-major reshape read `tensor.reshape_map (s_in, s_out)` — the map that reads a
/// PACKED producer (its output strip-mined to `s_in`) at the unpacked coordinates `s_out`?
/// Decided by normalization: both `map` and the canonical unpack map are applied to the same probe
/// variable; the reduced bodies are hash-consed, so pointer equality decides alpha-equivalence.
static bool
is_unpack_read(World& w, const Def* map, const Def* r_in, const Def* s_in, const Def* r_out, const Def* s_out) {
    auto pi = map->type()->isa<Pi>();
    if (!pi) return false;
    auto expected = w.app(w.app(w.annex<tensor::reshape_map>(), {r_in, r_out}), {s_in, s_out});
    auto epi      = expected->type()->isa<Pi>();
    if (!epi || epi->dom() != pi->dom() || epi->codom() != pi->codom()) return false;
    auto probe = w.mut_lam(pi->dom(), pi->codom()); // scratch binder: its var stands in for the cell vector
    return w.app(map, probe->var()) == w.app(expected, probe->var());
}

// Is `d` (a new-world map_reduce) consumed by exactly one node? Resolved via the old-world app it
// replaces; `new2old_` covers every map_reduce the phase has rewritten. Defs in `shared_` gained
// consumers by a read-through of a shared read, which the old-world count cannot see.
bool Fuse::sole_consumer(const Def* d) const {
    if (shared_.contains(d)) return false;
    auto old_it = new2old_.find(d);
    if (old_it == new2old_.end()) return false;
    auto cnt = mr_consumers_.find(old_it->second);
    return cnt != mr_consumers_.end() && cnt->second == 1;
}

// Fuses a trailing elementwise map into the reducing `tensor.map_reduce` producing one of its
// inputs — the reverse of `fuse_map_reduce` and the direction the producer gate deliberately
// rejects for `Rr > 0`: a reduction must not be inlined into a consumer's combiner (it would rerun
// per fold step), but it *can* absorb the consumer into its per-output-cell `post` epilogue (the
// GEMM-epilogue pattern, e.g. `relu(add(conv, bias))`).
//
// The producer input `k0` must be read elementwise over the map's whole domain (identity access
// map, `Sis#k0 = So`, with the map's `Rr = 0`, `Sr = So` and identity `map_out`) — anything else
// changes coordinates or drops cells and would need map inversion. All *other* inputs of the map —
// and the map's own epilogue inputs — become epilogue inputs of the producer, read once per output
// cell at their (arbitrary) access maps: those maps take the map's loop vector, which under the
// identity-read gate is exactly the producer's output-cell coordinate system.
//
// Result: the producer with
//   post    := (x, extras) ↦ post_o(f_o(init_o, extras[k0 ↦ post_i(x, extras_i)]), extras_o)
//   post_is := inner post_is ++ outer is \ k0 ++ outer post_is    (maps concatenated likewise)
//
// The producer must be consumed by this map alone (checked via its old-world consumer count):
// otherwise it stays materialized for the other consumers and additionally runs fused —
// duplicating the whole reduction loop nest.
//
// `callee`/`arg` are new-world (already rewritten): the caller iterates this on freshly fused apps.
const Def* Fuse::fuse_epilogue(const App* callee, const Def* arg) {
    auto mr = btensor::MapRedPost::of(callee);

    auto& w = new_world();

    auto nis_lit = Lit::isa<u64>(mr.nis);
    auto nps_lit = Lit::isa<u64>(mr.nps);
    auto ro_lit  = Lit::isa<u64>(mr.Ro);
    auto rn_lit  = Lit::isa<u64>(mr.Rn);
    if (!nis_lit || !nps_lit || !ro_lit || !rn_lit || *ro_lit != *rn_lit) return nullptr;
    auto nis_nat = *nis_lit;
    auto nps_nat = *nps_lit;
    if (nis_nat == 0) return nullptr;
    if (mr.Sr != mr.So) return nullptr;

    if (!is_identity_map(mr.map_out)) return nullptr;

    // A consumer that computes nothing (a pure re-indexed read) is read-through's job — sinking it
    // would re-wrap the producer forever (the unpack path below emits exactly such a copy on top).
    if (nis_nat == 1 && nps_nat == 0 && is_copy_comb(mr.comb) && is_identity_post(mr.post)) return nullptr;

    auto [is, ps] = arg->projs<2>();

    // Find the producer: the first input that is a map_reduce read elementwise over the whole
    // domain — at identity coordinates (input shape == So), or through the row-major reshape that
    // unpacks a strip-mined (packed) producer — and consumed by this map alone.
    u64 k0               = 0;
    const Def* inner_def = nullptr;
    bool unpack          = false;
    for (u64 k = 0; k < nis_nat && !inner_def; ++k) {
        auto cand = Axm::isa<tensor::map_reduce_post>(is->proj(nis_nat, k));
        if (!cand || !sole_consumer(cand) || !reads_as_produced(cand, mr.Sis->proj(nis_nat, k))) continue;

        auto m = mr.maps->proj(nis_nat, k);
        if (is_identity_map(m) && mr.Sis->proj(nis_nat, k) == mr.So) {
            k0        = k;
            inner_def = cand;
        } else if (mr.Sis->proj(nis_nat, k) != mr.So
                   && is_unpack_read(w, m, mr.Ris->proj(nis_nat, k), mr.Sis->proj(nis_nat, k), mr.Ro, mr.So)) {
            k0        = k;
            inner_def = cand;
            unpack    = true;
        }
    }
    if (!inner_def) return nullptr;

    // Reading through the unpack: the fused epilogue runs at the producer's PACKED cell
    // coordinates, so every map carried over from this consumer — its other combiner inputs and
    // its own epilogue inputs, all taking unpacked cell coordinates — is composed behind the
    // inverse reshape (packed cell → unpacked cell).
    const Def* to_cells = nullptr;
    if (unpack)
        to_cells = w.app(w.app(w.annex<tensor::reshape_map>(), {mr.Ro, mr.Ris->proj(nis_nat, k0)}),
                         {mr.So, mr.Sis->proj(nis_nat, k0)});
    auto inner = Axm::isa<tensor::map_reduce_post>(inner_def);

    auto im           = btensor::MapRedPost::of(inner->callee()->as<App>());
    auto [i_is, i_ps] = inner->arg()->projs<2>();

    auto i_nps_lit = Lit::isa<u64>(im.nps);
    if (!i_nps_lit) return nullptr;
    auto i_nps_nat = *i_nps_lit;

    log().d("fuse trailing map {} {} into the epilogue of {}", callee, arg, inner_def);

    // Concatenated epilogue inputs: the inner's own, then the map's other combiner inputs, then the
    // map's epilogue inputs. The maps' access maps carry over verbatim — their domain (the map's
    // loop vector) is the producer's output-cell coordinate system.
    auto new_nps = i_nps_nat + (nis_nat - 1) + nps_nat;
    auto to_cell = [&](const Def* m) { return to_cells ? compose_map(w, m, to_cells) : m; };
    auto eps     = Slots();
    for (u64 j = 0; j < i_nps_nat; ++j)
        eps.push(im.Tps->proj(i_nps_nat, j), im.Rps->proj(i_nps_nat, j), im.Sps->proj(i_nps_nat, j),
                 im.post_maps->proj(i_nps_nat, j), i_ps->proj(i_nps_nat, j));
    for (u64 i = 0; i < nis_nat; ++i)
        if (i != k0)
            eps.push(mr.Tis->proj(nis_nat, i), mr.Ris->proj(nis_nat, i), mr.Sis->proj(nis_nat, i),
                     to_cell(mr.maps->proj(nis_nat, i)), is->proj(nis_nat, i));
    for (u64 j = 0; j < nps_nat; ++j)
        eps.push(mr.Tps->proj(nps_nat, j), mr.Rps->proj(nps_nat, j), mr.Sps->proj(nps_nat, j),
                 to_cell(mr.post_maps->proj(nps_nat, j)), ps->proj(nps_nat, j));

    // The composed epilogue as a CPS chain: inner post → map combiner → map post. Identity hops are
    // called through anyway — their always-true filters inline them.
    auto fused_post              = w.mut_con({w.sigma({im.To, w.sigma(eps.T)}), w.cn(mr.Tp)})->set("fused_post");
    auto after_ip                = w.mut_con(im.Tp)->set("afterInnerPost");
    auto after_comb              = w.mut_con(mr.To)->set("afterComb");
    auto [fused_data, fused_ret] = fused_post->vars<2>();
    auto [x, extras]             = fused_data->projs<2>();

    DefVec i_extras(i_nps_nat, [&](size_t j) { return extras->proj(new_nps, j); });
    fused_post->app(true, im.post, {w.tuple({x, w.tuple(i_extras)}), after_ip});

    DefVec comb_inputs(nis_nat);
    for (u64 i = 0, r = 0; i < nis_nat; ++i)
        comb_inputs[i] = i == k0 ? after_ip->var() : extras->proj(new_nps, i_nps_nat + r++);
    after_ip->app(true, mr.comb, {w.tuple({mr.init, w.tuple(comb_inputs)}), after_comb});

    DefVec o_extras(nps_nat, [&](size_t j) { return extras->proj(new_nps, i_nps_nat + (nis_nat - 1) + j); });
    after_comb->app(true, mr.post, {w.tuple({after_comb->var(), w.tuple(o_extras)}), fused_ret});

    // The producer, keeping its loop nest untouched; only the epilogue (inputs) and the out-element
    // type change.
    auto res = w.annex<tensor::map_reduce_post>();
    res      = w.app(res, {im.nis, w.lit_nat(new_nps)});
    res      = w.app(res, {im.To, mr.Tp, im.Ro, im.Rn, im.TSched});
    res      = w.app(res, im.shapes);
    res      = w.app(res, {im.Tis, im.Ris, im.Sis, w.tuple(eps.T), w.tuple(eps.R), w.tuple(eps.S)});
    res      = w.app(res, {im.comb, im.init, fused_post});
    res      = w.app(res, im.map_out);
    res      = w.app(res, {im.maps, w.tuple(eps.maps)});
    res      = w.app(res, {i_is, w.tuple(eps.is)});

    if (unpack) {
        // The fused op still materializes PACKED; re-wrap it in the unpacking reshape — expanded
        // via the impl so the wrapper is again a pure copy-mr that consumers read through (or a
        // plain copy at a graph boundary). The copy-consumer guard above keeps this wrapper from
        // being sunk right back.
        auto impl = w.annex<tensor::reshape_impl>();
        impl      = w.app(impl, {mr.Tp, mr.Ris->proj(nis_nat, k0), mr.Ro});
        impl      = w.app(impl, mr.Sis->proj(nis_nat, k0));
        impl      = w.app(impl, mr.So);
        res       = w.app(impl, res);
    }

    return res;
}

// Rewires every input slot — combiner and epilogue alike — that is a pure re-indexed read
// (a copy-combiner map_reduce or a `tensor.broadcast`, see `read_through`) to read the underlying
// source directly, with the read's access map composed behind the slot's map. This absorbs
// reshape/transpose/slice/flip/repeat/broadcast chains into the access maps of the consuming
// map_reduce, so they never materialize; the bypassed op dies with cleanup unless someone else
// still reads it (in which case the rewiring is still free — it removes no sharing, only a copy).
const Def* Fuse::fuse_read_through(const App* callee, const Def* arg) {
    auto mr = btensor::MapRedPost::of(callee);

    auto nis_lit = Lit::isa<u64>(mr.nis);
    auto nps_lit = Lit::isa<u64>(mr.nps);
    if (!nis_lit || !nps_lit) return nullptr;
    auto nis_nat = *nis_lit;
    auto nps_nat = *nps_lit;

    auto& w = new_world();

    auto [is, ps] = arg->projs<2>();

    bool changed = false;
    auto rewire  = [&](u64 n, const Def* Ts, const Def* Rs, const Def* Ss, const Def* ms, const Def* vs) {
        auto slots = Slots();
        for (u64 i = 0; i < n; ++i) {
            auto T = Ts->proj(n, i), R = Rs->proj(n, i), S = Ss->proj(n, i);
            auto m = ms->proj(n, i), v = vs->proj(n, i);
            if (auto rt = read_through(w, v, m, S)) {
                log().d("read input {} of {} through {}", i, callee, v);
                // The bypassed read forwards its consumers to the source: a shared (or uncounted,
                // e.g. broadcast) read leaves the source multiply-consumed for the epilogue guard.
                if (!sole_consumer(v)) shared_.insert(rt->src);
                T = rt->T, R = rt->R, S = rt->S, m = compose_map(w, rt->map, m), v = rt->src;
                changed = true;
            }
            slots.push(T, R, S, m, v);
        }
        return slots;
    };

    auto ins = rewire(nis_nat, mr.Tis, mr.Ris, mr.Sis, mr.maps, is);
    auto eps = rewire(nps_nat, mr.Tps, mr.Rps, mr.Sps, mr.post_maps, ps);
    if (!changed) return nullptr;

    auto res = w.annex<tensor::map_reduce_post>();
    res      = w.app(res, mr.nis_nps);
    res      = w.app(res, mr.meta);
    res      = w.app(res, mr.shapes);
    res = w.app(res, {w.tuple(ins.T), w.tuple(ins.R), w.tuple(ins.S), w.tuple(eps.T), w.tuple(eps.R), w.tuple(eps.S)});
    res = w.app(res, mr.comb_init);
    res = w.app(res, mr.map_out);
    res = w.app(res, {w.tuple(ins.maps), w.tuple(eps.maps)});
    res = w.app(res, {w.tuple(ins.is), w.tuple(eps.is)});

    return res;
}

namespace {

bool is_mr(const Def* d) { return static_cast<bool>(Axm::isa<tensor::map_reduce_post>(d)); }

} // namespace

void Fuse::start() {
    // The epilogue direction needs to know whether a map_reduce is consumed by exactly one other node.
    mr_consumers_ = count_consumers(old_world(), is_mr);
    RWPhase::start();
}

const Def* Fuse::rewrite_imm_App(const App* app) {
    if (Axm::isa<tensor::map_reduce_post>(app)) {
        const App* cur = nullptr;
        if (auto res = fuse_map_reduce(app)) {
            log().d("fused map_reduce {} → {}", app, res);
            cur = res->as<App>();
        }
        // Read-throughs and the epilogue direction, to a fixpoint: each round may absorb pure
        // re-indexed reads into access maps or sink the current map — first the plainly rewritten
        // one, then the producer-fused result, then each fused result — into the producer of one
        // of its inputs. Terminates quickly: rewiring walks strictly down the producer DAG, and a
        // fused reduction fails the Rr = 0 outer gate on the next round.
        auto callee = cur ? cur->callee()->as<App>() : rewrite(app->callee())->as<App>();
        auto arg    = cur ? cur->arg() : rewrite(app->arg());
        for (bool progress = true; progress;) {
            progress       = false;
            const Def* res = fuse_read_through(callee, arg);
            if (!res) {
                res = fuse_epilogue(callee, arg);
                if (res) log().d("fused trailing map {} into its producer's epilogue → {}", app, res);
            }
            if (res) {
                cur      = res->as<App>();
                callee   = cur->callee()->as<App>();
                arg      = cur->arg();
                progress = true;
            }
        }
        auto result = cur ? cur : RWPhase::rewrite_imm_App(app);
        // Remember which old app this (possibly fused) map_reduce replaces, so later epilogue
        // rounds of its consumers can look up its consumer count.
        if (is_mr(result)) {
            new2old_[result] = app;
            // An epilogue sink into a PACKED producer wraps the fused op in an unpack copy (see
            // fuse_epilogue). Register the wrapped producer under the same old app — its sole
            // consumer is the wrapper, which stands for this app — so the NEXT trailing map (which
            // producer-fuses the wrapper away and reads the packed op directly) can sink too.
            if (auto pr = is_pure_read(result); pr && is_mr(pr->src) && !new2old_.contains(pr->src))
                new2old_[pr->src] = app;
        }
        return result;
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
