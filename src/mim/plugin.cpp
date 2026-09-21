#include "mim/plugin.h"

namespace mim {

std::optional<plugin_t> Annex::mangle(std::string_view plugin) {
    auto n = plugin.size();
    if (n > Max_Plugin_Size) return {};

    u64 result = 0;
    for (size_t i = 0; i != Max_Plugin_Size; ++i) {
        u64 u = '\0';

        if (i < n) {
            auto c = plugin[i];
            if (c == '_')
                u = 1;
            else if ('a' <= c && c <= 'z')
                u = c - 'a' + 2_u64;
            else if ('A' <= c && c <= 'Z')
                u = c - 'A' + 28_u64;
            else if ('0' <= c && c <= '9')
                u = c - '0' + 54_u64;
            else
                return {};
        }

        result = (result << 6_u64) | u;
    }

    return result << 16_u64;
}

std::string Annex::demangle(plugin_t plugin) {
    std::string result;
    for (size_t i = 0; i != Max_Plugin_Size; ++i) {
        u64 c = (plugin & 0xfc00000000000000_u64) >> 58_u64;
        if (c == 0)
            return result;
        else if (c == 1)
            result += '_';
        else if (2 <= c && c < 28)
            result += 'a' + ((char)c - 2);
        else if (28 <= c && c < 54)
            result += 'A' + ((char)c - 28);
        else
            result += '0' + ((char)c - 54);

        plugin <<= 6_u64;
    }

    return result;
}

} // namespace mim
