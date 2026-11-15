#include "mim/ast/tok.h"

#include <fe/assert.h>

namespace mim::ast {

const char* Tok::tag2str(Tok::Tag tag) {
    switch (tag) {
#define CODE(t, str) \
    case Tok::Tag::t: return str;
        MIM_KEY(CODE)
        MIM_TOK(CODE)
#undef CODE
        case Tag::Nil: std::unreachable();
    }
    std::unreachable();
}

std::string Tok::str() const {
    std::ostringstream oss;
    oss << *this;
    return oss.str();
}

} // namespace mim::ast
