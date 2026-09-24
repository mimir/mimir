#include "mim/phase/nom_erasure.h"

namespace mim {

const Def* NomErasure::rewrite_imm_Nom(const Nom* nom) {
    auto type = rewrite(nom->op());
    log().d("nom-erasure `{}` → `{}`", nom, type);
    profile_count("nominals eliminated");
    return type;
}

const Def* NomErasure::rewrite_imm_Wrap(const Wrap* wrap) {
    profile_count("nominals eliminated");
    return rewrite(wrap->value());
}

const Def* NomErasure::rewrite_imm_Unwrap(const Unwrap* unwrap) {
    profile_count("nominals eliminated");
    return rewrite(unwrap->value());
}

} // namespace mim
