#pragma once

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>

#include "mim/plug/x86/autogen.h"

/// Prefix for this backend's diagnostics; concatenate it with the format literal.
#define MIM_X86_BE "x86 backend: "

namespace mim::plug::x86 {

inline bool is_mem(const Def* def) {
    auto type = def->type(); // Level has no type
    return type && Axm::isa<core::M>(type);
}

/// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
inline const Def* prev_mem(const App* app) {
    if (Axm::isa<sched>(app)) return app->callee()->as<App>()->arg();
    if (Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<slot>(app) || Axm::isa<rm>(app) || Axm::isa<mr>(app))
        return app->arg(app->num_args(), 0);
    return nullptr;
}

} // namespace mim::plug::x86
