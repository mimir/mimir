#include "mim/driver.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

#include "mim/plugin.h"

#include "mim/util/dl.h"
#include "mim/util/sys.h"

namespace mim {

std::pair<const fs::path*, bool> Driver::Imports::add(fs::path path, Sym sym, ast::Tok::Tag tag) {
    if (!fs::exists(path)) {
        driver_.WLOG("import path '{}' does not exist", path.string());
        return {nullptr, false};
    }

    const fs::path* imported_path = nullptr;
    bool fresh                    = true;
    for (const auto& parsed_path : parsed_paths_) {
        if (fs::equivalent(parsed_path, path)) {
            imported_path = &parsed_path;
            fresh         = false;
            break;
        }
    }

    if (!imported_path) {
        parsed_paths_.emplace_back(std::move(path));
        imported_path = &parsed_paths_.back();
    }

    bool seen_entry = false;
    for (const auto& entry : entries_) {
        if (entry.sym == sym && entry.tag == tag && fs::equivalent(entry.path, *imported_path)) {
            seen_entry = true;
            break;
        }
    }

    if (!seen_entry) entries_.emplace_back(Entry{*imported_path, sym, tag});
    return {imported_path, fresh};
}

Driver::Driver()
    : version_(MIM_VERSION)
    , log_(flags_)
    , world_(this)
    , imports_(*this) {
    // prepend empty path
    search_paths_.emplace_front(fs::path{});

    // paths from env
    if (auto env_path = std::getenv("MIM_PLUGIN_PATH")) {
        std::stringstream env_path_stream{env_path};
        std::string sub_path;
        while (std::getline(env_path_stream, sub_path, ':'))
            add_search_path(sub_path);
    }

    // add <path/to/libmim>/mim
    if (auto path = sys::path_to_libmim()) add_search_path(path->parent_path() / "mim");

    // add install path if different from above
    if (auto install_path = fs::path{MIM_INSTALL_PREFIX} / MIM_LIBDIR / "mim"; fs::exists(install_path)) {
        if (search_paths().size() < 2 || !fs::equivalent(install_path, search_paths().back()))
            add_search_path(std::move(install_path));
    }

    // all other user paths are placed just behind the first path (the empty path)
    insert_ = ++search_paths_.begin();
}

void Driver::load(Sym name) {
    ILOG("💾 loading plugin: '{}'", name);

    if (is_loaded(name)) {
        WLOG("mim/plugin '{}' already loaded", name);
        return;
    }

    auto handle = Plugin::Handle{nullptr, dl::close};
    if (auto path = fs::path{name.view()}; path.is_absolute() && fs::is_regular_file(path))
        handle.reset(dl::open(name.c_str()));
    if (!handle) {
        for (const auto& path : search_paths()) {
            auto full_path = path / std::format("libmim_{}.{}", name, dl::extension);
            std::error_code ignore;
            if (bool reg_file = fs::is_regular_file(full_path, ignore); reg_file && !ignore) {
                auto path_str = full_path.string();
                if (handle.reset(dl::open(path_str.c_str())); handle) break;
            }
            if (handle) break;
        }
    }

    if (!handle) error("cannot open plugin '{}'", name);

    if (auto get_info = reinterpret_cast<decltype(&mim_get_plugin)>(dl::get(handle.get(), "mim_get_plugin"))) {
        auto plugin = get_info();
        if (version() != plugin.version) {
            std::ostringstream oss;
            std::print(oss, "plugin {} has version {} while MimIR has version {}", plugin.name, plugin.version,
                       version());
            if (flags().force_load)
                std::cerr << "warning: " << oss.str() << '\n';
            else
                throw std::logic_error(oss.str());
        }
        assert_emplace(plugins_, name, std::move(handle));
        // clang-format off
        if (auto reg = plugin.register_normalizers) reg(normalizers_);
        if (auto reg = plugin.register_stages)      reg(stages_);
        if (auto reg = plugin.register_backends)    reg(backends_);
        // clang-format on
    } else {
        error("mim/plugin has no 'mim_get_plugin()'");
    }
}

void* Driver::get_fun_ptr(Sym plugin, const char* name) {
    if (auto handle = lookup(plugins_, plugin)) return dl::get(handle->get(), name);
    return nullptr;
}

} // namespace mim
