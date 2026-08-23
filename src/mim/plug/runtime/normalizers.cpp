#include "mim/tuple.h"
#include "mim/world.h"

#include "mim/plug/runtime/runtime.h"

namespace mim::plug::runtime {

const Def* normalize_static_check(const Def* type, const Def* callee, const Def* arg) {
    auto& w = type->world();
    auto [condition, message] = arg->projs<2>();
    if (condition == w.lit_tt()) return w.lit_tt();
    if (condition == w.lit_ff()) {
        mim::error(callee->loc(), "static runtime assertion failed: {}", tuple2str(message));
        return w.bot(type);
    }
    return nullptr;
}

MIM_runtime_NORMALIZER_IMPL

} // namespace mim::plug::runtime
