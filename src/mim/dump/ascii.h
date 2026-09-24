#pragma once

#include <string>
#include <string_view>

namespace mim::dump {

/// The Mim text @p utf8 with its Unicode tokens transliterated for a terminal that cannot show them.
std::string ascii(std::string_view utf8);

} // namespace mim::dump
