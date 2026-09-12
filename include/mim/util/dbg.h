#pragma once

#include <string>
#include <string_view>

#include <fe/dbg.h>
#include <fe/error.h>
#include <fe/loc.h>
#include <fe/sym.h>

namespace mim {

using fe::Dbg;
using fe::DbgKey;
using fe::Error;
using fe::Loc;
using fe::Pos;
using fe::Sym;

/// Escapes each `` ` `` of @p str as `` \` ``, which is how a diagnostic spells one that must not cite.
inline std::string cite(std::string_view str) {
    if (str.find('`') == std::string_view::npos) return std::string(str);

    auto res = std::string();
    for (auto c : str) {
        if (c == '`') res += '\\';
        res += c;
    }
    return res;
}

} // namespace mim
