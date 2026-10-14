#include "mim/plugin.h"

namespace mim {

namespace {
constexpr auto Alphabet = std::string_view("_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
}

std::optional<plugin_t> Annex::mangle(std::string_view plugin) {
    auto n = plugin.size();
    if (n > Max_Plugin_Size) return {};

    u64 result = 0;
    for (size_t i = 0; i != Max_Plugin_Size; ++i) {
        u64 u = '\0';

        if (i < n) {
            auto pos = Alphabet.find(plugin[i]);
            if (pos == std::string_view::npos) return {};
            u = pos + 1;
        }

        result = (result << 6_u64) | u;
    }

    return result << 16_u64;
}

std::string Annex::demangle(plugin_t plugin) {
    std::string result;
    for (size_t i = 0; i != Max_Plugin_Size; ++i) {
        u64 c = (plugin & 0xfc00000000000000_u64) >> 58_u64;
        if (c == 0) return result;
        result += Alphabet[c - 1];

        plugin <<= 6_u64;
    }

    return result;
}

} // namespace mim
