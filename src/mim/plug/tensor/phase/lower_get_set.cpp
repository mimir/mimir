#include "mim/plug/tensor/phase/lower_get_set.h"

#include <mim/def.h>
#include <mim/lam.h>

#include <mim/util/types.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

const Def* LowerGetSet::lower_get(const App* app) {
    auto& w           = new_world();
    auto [T, r, s]    = rewrite(app->callee())->as<App>()->args<3>();
    auto [index, arr] = rewrite(app->arg())->projs<2>();

    log().d("lower get: arr = {}: {}, index = {}, T = {}, r = {}, s = {}", arr, arr->type(), index, T, r, s);

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        log().w("rank {} of {} is not known at lowering time", r, app);
        return nullptr;
    }

    for (auto ri = 0_u64; ri != *r_nat; ++ri)
        arr = w.extract(arr, index->proj(*r_nat, ri));
    return arr;
}

const Def* LowerGetSet::lower_set(const App* app) {
    auto& w              = new_world();
    auto [T, r, s]       = rewrite(app->callee())->as<App>()->args<3>();
    auto [index, arr, x] = rewrite(app->arg())->projs<3>();

    log().d("lower set: arr = {}: {}, index = {}, x = {}: {}, T = {}, r = {}, s = {}", arr, arr->type(), index, x,
            x->type(), T, r, s);

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        log().w("rank {} of {} is not known at lowering time", r, app);
        return nullptr;
    }

    // The sub-array each level inserts into; `nested[0]` is `arr` itself (rank 0 is normalized away).
    DefVec nested(*r_nat);
    nested[0] = arr;
    for (auto ri = 1_u64; ri != *r_nat; ++ri)
        nested[ri] = w.extract(nested[ri - 1], index->proj(*r_nat, ri - 1));

    for (auto ri = *r_nat; ri-- != 0;)
        x = w.insert(nested[ri], index->proj(*r_nat, ri), x);
    return x;
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
