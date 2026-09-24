#include "ascii.h"

#include <array>
#include <utility>

namespace mim::dump {

std::string ascii(std::string_view utf8) {
    using namespace std::literals;
    static constexpr auto table = std::array{
        std::pair{"→"sv,     "->"sv},
        std::pair{"←"sv,     "<-"sv},
        std::pair{"«"sv,     "<<"sv},
        std::pair{"»"sv,     ">>"sv},
        std::pair{"‹"sv,     "(<"sv},
        std::pair{"›"sv,     ">)"sv},
        std::pair{"⊥"sv,    "bot"sv},
        std::pair{"⊤"sv,    "top"sv},
        std::pair{"λ"sv,     "lm"sv},
        std::pair{"□"sv, "Type 1"sv},
    };
    auto res = std::string();
    res.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        auto rest = utf8.substr(i);
        auto hit  = false;
        for (auto [from, to] : table)
            if (rest.starts_with(from)) {
                res += to;
                i += from.size();
                hit = true;
                break;
            }
        if (!hit) res += utf8[i++];
    }
    return res;
}

} // namespace mim::dump
