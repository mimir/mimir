#include "mim/phase/unload.h"

#include "mim/driver.h"

namespace mim {

void Unload::apply(std::string plugin) {
    plugin_str_ = std::move(plugin);
    name_ += " \"" + plugin_str_ + "\"";
    if (auto mangled = Annex::mangle(plugin_str_))
        plugin_ = *mangled;
    else
        fe::throwf("invalid plugin name `{}`", plugin_str_);
}

void Unload::start() {
    world().annexes().detach(plugin_);
    // we could also literally unload the shared object now, but it's probably not worth the trouble
}

} // namespace mim
