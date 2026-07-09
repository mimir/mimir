#include <absl/container/flat_hash_set.h>

#include "mim/world.h"

#include "mim/plug/scf/autogen.h"

namespace mim::plug::scf {

const Def* normalize_switch(const Def*, const Def*, const Def* cases) {
    absl::flat_hash_set<nat_t> labels;
    for (auto c : cases->projs()) {
        auto index = c->proj(2, 0);
        if (auto l = Lit::isa(index))
            if (!labels.insert(*l).second) mim::error(c->loc(), "duplicate case label '{}' in switch", *l);
    }

    return {};
}

MIM_scf_NORMALIZER_IMPL

} // namespace mim::plug::scf
