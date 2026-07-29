#include <mim/driver.h>
#include <mim/world.h>

#include "mim/plug/compile/compile.h"

namespace mim::plug::compile {

const Def* normalize_is_loaded(const Def*, const Def*, const Def* arg) {
    auto& world  = arg->world();
    auto& driver = world.driver();
    if (auto str = tuple2str(arg); !str.empty()) return world.lit_bool(driver.is_loaded(driver.sym(str)));

    return {};
}

/// `%compile.aggr fallback` ↦ `tt`/`ff` if `-X compile:aggr=on`/`=off` was passed, else `fallback`.
const Def* normalize_aggr(const Def*, const Def*, const Def* arg) {
    auto& world  = arg->world();
    auto& driver = world.driver();
    for (const auto& a : driver.args(driver.sym("compile"))) {
        // clang-format off
        if (a == "aggr=tt" || a == "aggr=on"  || a == "aggr=true" || a == "aggr") return world.lit_tt();
        if (a == "aggr=ff" || a == "aggr=off" || a == "aggr=false")               return world.lit_ff();
        // clang-format on
    }
    return arg;
}

/// `%compile.cond name phase` ↦ `phase` if `name`'s plugin is loaded, else `%compile.null`.
const Def* normalize_cond(const Def*, const Def* callee, const Def* phase) {
    auto& world  = phase->world();
    auto& driver = world.driver();
    auto name    = callee->as<App>()->arg();
    if (auto str = tuple2str(name); !str.empty() && driver.is_loaded(driver.sym(str))) return phase;
    return world.annex(Annex::base<null>());
}

MIM_compile_NORMALIZER_IMPL

} // namespace mim::plug::compile
