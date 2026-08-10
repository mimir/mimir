#include "mim/plug/tensor/phase/lower_get_set.h"

#include <mim/def.h>
#include <mim/lam.h>

#include <mim/util/types.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

const Def* LowerGetSet::lower_get(const App* app) {
    auto& w  = new_world();
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [arr, index] = arg->projs<2>();
    auto callee       = c->as<App>();
    auto [T, r, s]    = callee->args<3>();

    DLOG("lower_get");
    DLOG("    arr = {} : {}", arr, arr->type());
    if (auto arr_seq = arr->type()->isa<Seq>()) DLOG("    arr shape = {}", arr_seq->arity());
    DLOG("    index = {} : {}", index, index->type());
    DLOG("    T = {} : {}", T, T->type());
    DLOG("    r = {} : {}", r, r->type());
    DLOG("    s = {} : {}", s, s->type());

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        WLOG("{} doesn't have a lowering-time known rank: {}", app, r);
        return nullptr;
    }
    if (r_nat == 1) {
        DLOG("index of size 1, extract");
        return w.extract(arr, index);
    }
    auto curr_arr = arr;
    for (auto ri = 0_u64; ri < *r_nat; ++ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    idx = {} : {}", idx, idx->type());
        curr_arr = w.extract(curr_arr, idx);
    }
    return curr_arr;
}

const Def* LowerGetSet::lower_set(const App* app) {
    auto& w  = new_world();
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [arr, index, x] = arg->projs<3>();

    DLOG("lower_set");
    DLOG("    arr = {} : {}", arr, arr->type());
    DLOG("    index = {} : {}", index, index->type());
    DLOG("    x = {} : {}", x, x->type());

    auto callee    = c->as<App>();
    auto [T, r, s] = callee->args<3>();
    DLOG("    T = {} : {}", T, T->type());
    DLOG("    r = {} : {}", r, r->type());
    DLOG("    s = {} : {}", s, s->type());

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        WLOG("{} doesn't have a lowering-time known rank: {}", app, r);
        return nullptr;
    }
    if (r_nat == 1) {
        DLOG("index of size 1, insert");
        return w.insert(arr, index, x);
    }

    // r_nat will never be 0, as we would have normalized this case away already
    DefVec arrs_to_insert_into(*r_nat);
    arrs_to_insert_into[0] = arr;
    for (auto ri = 0_u64; ri < *r_nat - 1; ++ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    extract idx = {} : {}", idx, idx->type());
        arrs_to_insert_into[ri + 1] = w.extract(arrs_to_insert_into[ri], idx);
    }

    auto new_arr = x;
    for (auto ri = static_cast<s64>(*r_nat - 1); ri >= 0; --ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    idx = {} : {}", idx, idx->type());
        DLOG("    arr_to_insert_into = {} : {}", arrs_to_insert_into[ri], arrs_to_insert_into[ri]->type());

        new_arr = w.insert(arrs_to_insert_into[ri], idx, new_arr);
    }
    return new_arr;
}

const Def* LowerGetSet::rewrite_imm_App(const App* app) {
    if (auto get = Axm::isa<tensor::get>(app)) {
        if (auto res = lower_get(get)) return res;
    } else if (auto set = Axm::isa<tensor::set>(app)) {
        if (auto res = lower_set(set)) return res;
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
