#include "mim/phase/nom_erasure.h"

namespace mim {

const Def* NomErasure::rewrite_mut_Nom(Nom* nom) {
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
