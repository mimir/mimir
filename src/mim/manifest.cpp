#include "mim/manifest.h"

#include <cstdlib>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fe/sys.h>

namespace mim {

namespace {

std::string_view trim(std::string_view s) {
    auto beg = s.find_first_not_of(" \t\r");
    return beg == std::string_view::npos ? std::string_view{} : s.substr(beg, s.find_last_not_of(" \t\r") - beg + 1);
}

std::vector<std::string> split_list(std::string_view s) {
    return std::views::split(s, ',')
         | std::views::transform([](auto&& r) { return std::string(trim({r.begin(), r.end()})); })
         | std::views::filter([](auto&& item) { return !item.empty(); }) | std::ranges::to<std::vector>();
}

struct List_Entry {
    std::string_view key;
    std::vector<std::string> Manifest::* list;
};

constexpr auto lists = std::array{
    List_Entry{     Plugin_Key,      &Manifest::plugins},
    List_Entry{Plugin_Path_Key, &Manifest::plugin_paths},
    List_Entry{Import_Path_Key, &Manifest::import_paths},
    List_Entry{Prefix_Path_Key, &Manifest::prefix_paths},
    List_Entry{ Plugin_Arg_Key,  &Manifest::plugin_args}
};
} // namespace

namespace fs = std::filesystem;

std::optional<fs::path> find_manifest(const fs::path& start_in) {
    std::error_code ec{};
    auto start = start_in.empty() ? fs::current_path(ec) : fs::absolute(start_in, ec);
    if (ec) return std::nullopt;

    auto is_file = fs::is_regular_file(start, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return std::nullopt;
    auto walk = is_file ? start.parent_path() : start;

    if (walk.empty()) {
        walk = fs::current_path(ec);
        if (ec) return std::nullopt;
    }

    for (;;) {
        if (fs::is_regular_file(walk / Manifest_File, ec)) return walk / Manifest_File;
        if (fs::exists(walk / ".git", ec)) return std::nullopt;

        auto parent = walk.parent_path();
        if (parent == walk || parent.empty()) return std::nullopt;
        walk = std::move(parent);
    }
}

Manifest load_manifest(const fs::path& file) {
    std::error_code ec{};
    if (!fs::is_regular_file(file, ec))
        throw std::invalid_argument("error: cannot read config file `" + file.string() + "`");

    std::ifstream in{file};
    if (!in) throw std::invalid_argument("error: cannot read config file `" + file.string() + "`");

    Manifest manifest{};
    std::string line{};
    size_t row = 0;
    while (std::getline(in, line)) {
        ++row;
        auto fail = [&](std::string_view msg) {
            throw std::invalid_argument("error: " + file.string() + ":" + std::to_string(row) + ": "
                                        + std::string(msg));
        };
        // `std::getline` is platform dependent, what if a Unix machine opens a Windows file?
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // drop everything after '#'
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        auto text = trim(line);
        if (text.empty()) continue;
        auto eq = text.find('=');
        if (eq == std::string_view::npos) fail("expected `key = value`");
        auto key = trim(text.substr(0, eq));
        auto val = trim(text.substr(eq + 1));

        auto entry = std::ranges::find(lists, key, &List_Entry::key);
        if (entry == lists.end()) fail("unknown manifest key `" + std::string(key) + "`");

        auto& dst = std::invoke(entry->list, &manifest);
        for (auto&& item : split_list(val)) {
            if (entry->list == &Manifest::plugin_args && !item.contains(':'))
                fail("expected `<plugin>:<arg>`, got `" + item + "`");
            dst.emplace_back(std::move(item));
        }
    }

    // Relative `*-path` entries anchor to the manifest file's directory.
    auto base = file.parent_path();
    if (base.empty()) base = ".";
    for (auto* paths : {&manifest.plugin_paths, &manifest.import_paths, &manifest.prefix_paths})
        for (auto&& p : *paths) {
            auto fp = fs::path(p);
            if (!fp.is_absolute()) p = (base / fp).lexically_normal().string();
        }
    return manifest;
}

std::vector<std::string> paths_from_env(const char* var) {
    if (auto env = std::getenv(var))
        return std::views::split(std::string_view(env), fe::sys::Path_Sep)
             | std::views::transform([](auto&& r) { return std::string(r.begin(), r.end()); })
             | std::views::filter([](auto&& p) { return !p.empty(); }) | std::ranges::to<std::vector>();
    return {};
}

} // namespace mim
