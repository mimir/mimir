#include "mim/phase/unload.h"

#include "mim/driver.h"

namespace mim {

void Unload::apply(std::string plugin) {
    plugin_str_ = plugin;
    name_ += " \"" + plugin_str_ + " \"";
    if (auto plugin = Annex::mangle(driver().sym(plugin_str_)))
        plugin_ = *Annex::mangle(driver().sym(plugin_str_));
    else
        fe::throwf("invalid plugin name `{}`", plugin_str_);
}

void Unload::start() {
    auto& flags2entry = world().annexes().flags2entry();
    auto& sym2flags   = world().annexes().sym2flags();
    for (auto i = flags2entry.begin(); i != flags2entry.end();) {
        auto [flags, entry] = *i;

        if (Annex::flags2plugin(flags) == plugin_) {
            i = flags2entry.erase(i);
            sym2flags.erase(entry.sym);
        } else
            ++i;
    }
}

} // namespace mim
