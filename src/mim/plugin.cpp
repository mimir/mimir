#include "mim/plugin.h"

#include "mim/driver.h"

using namespace std::literals;

namespace mim {

std::optional<plugin_t> Annex::mangle(Sym plugin) {
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

Sym Annex::demangle(Driver& driver, plugin_t plugin) {
    std::string result;
    for (size_t i = 0; i != Max_Plugin_Size; ++i) {
        u64 c = (plugin & 0xfc00000000000000_u64) >> 58_u64;
        if (c == 0)
            return driver.sym(result);
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

    return driver.sym(result);
}

std::tuple<Sym, Sym, Sym> Annex::split(Driver& driver, Sym s) {
    if (!s) return {};
    if (s[0] != '%') return {};
    auto sv = fe::subview(s, 1);

    auto dot = sv.find('.');
    if (dot == std::string_view::npos) return {};

    auto plugin = driver.sym(fe::subview(sv, 0, dot));
    if (!mangle(plugin)) return {};

    auto tag = fe::subview(sv, dot + 1);
    if (auto dot = tag.find('.'); dot != std::string_view::npos) {
        auto sub = driver.sym(fe::subview(tag, dot + 1));
        tag      = fe::subview(tag, 0, dot);
        return {plugin, driver.sym(tag), sub};
    }

    return {plugin, driver.sym(tag), {}};
}

} // namespace mim
