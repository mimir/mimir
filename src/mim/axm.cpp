#include "mim/axm.h"

#include "mim/world.h"

namespace mim {

Axm::Axm(NormalizeFn normalizer, u8 curry, u8 trip, const Def* type, plugin_t plugin, tag_t tag, sub_t sub)
    : Def(Node, type, Defs{}, Annex::flags(plugin, tag, sub)) {
    normalizer_ = normalizer;
    curry_      = curry;
    trip_       = trip;
}

std::pair<u8, u8> Axm::infer_curry_and_trip(const Def* type) {
    u8 curry = 0;
    u8 trip  = 0;
    MutSet done;
    while (auto pi = type->isa<Pi>()) {
        if (auto mut = pi->isa_mut()) {
            if (auto [_, ins] = done.emplace(mut); !ins) {
                // infer trip
                auto curr = pi;
                do {
                    ++trip;
                    curr = curr->codom()->as<Pi>();
                } while (curr != mut);
                break;
            }
        }

        ++curry;
        type = pi->codom();
    }

    return {curry, trip};
}

std::tuple<const Axm*, u8, u8> Axm::get(const Def* def) {
    if (auto axm = def->isa<Axm>()) return {axm, axm->curry(), axm->trip()};
    if (auto app = def->isa<App>()) return {app->axm(), app->curry(), app->trip()};
    return {nullptr, 0, 0};
}

} // namespace mim
