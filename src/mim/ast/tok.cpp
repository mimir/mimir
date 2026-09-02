#include "mim/ast/tok.h"

#include <fe/assert.h>

#include "mim/lam.h"
#include "mim/tuple.h"

namespace mim::ast {

const char* Tok::tag2str(Tok::Tag tag) {
    switch (tag) {
#define CODE(t, str) \
    case Tok::Tag::t: return str;
        MIM_KEY(CODE)
        MIM_TOK(CODE)
#undef CODE
        case Tag::Nil: fe::unreachable();
    }
    fe::unreachable();
}

std::string Tok::str() const {
    std::ostringstream oss;
    oss << *this;
    return oss.str();
}

/// @name std::ostream operator
///@{
std::ostream& operator<<(std::ostream& os, Tok tok) {
    if (tok.has_sym()) return os << tok.sym();
    return os << Tok::tag2str(tok.tag());
}
///@}

} // namespace mim::ast
