#include "mim/phase/foobar.h"

namespace mim {

bool Foobar::analyze() {
    for (auto def : old_world().annexes())
        analyze(def);
    for (auto def : old_world().externals().muts())
        analyze(def);

    return false; // no fixed-point neccessary
}

void Foobar::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    // If we want to match any axm defined in core here, we better move this Phase to core
    /*
    if (auto add = Axm::isa(plug::core::wrap::add, def)) {
        add->dump();
    }
    */

    for (auto d : def->deps())
        analyze(d);
}

} // namespace mim
