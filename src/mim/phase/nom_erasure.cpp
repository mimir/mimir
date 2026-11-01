#include "mim/phase/nom_erasure.h"

#include <fe/worklist.h>

namespace mim {

static bool has_nom(const Def* def) {
    for (auto queue = fe::BFSWorklist<DefSet>{def}; !queue.empty();) {
        auto d = queue.pop();
        if (d->isa<Nom>()) return true;
        for (auto dep : d->deps())
            queue.push(dep);
    }
    return false;
}

const Def* NomErasure::lookup(const Def* old_def) {
    // An opaque Axm whose type mentions a Nom is rebuilt under the same flags with an erased type.
    if (auto axm = old_def->isa<Axm>()) {
        if (auto new_def = Rewriter::lookup(axm)) return new_def;
        if (has_nom(axm->type())) return nullptr;
    }
    return RWPhase::lookup(old_def);
}

const Def* NomErasure::rewrite_imm_Nom(const Nom* nom) {
    auto type = rewrite(nom->op());
    log().d("nom-erasure `{}` → `{}`", nom, type);
    profile_count("nominals eliminated");
    return type;
}

const Def* NomErasure::rewrite_imm_Name(const Name* name) {
    profile_count("nominals eliminated");
    return rewrite(name->op());
}

const Def* NomErasure::rewrite_imm_Struc(const Struc* struc) {
    profile_count("nominals eliminated");
    return rewrite(struc->op());
}

} // namespace mim
