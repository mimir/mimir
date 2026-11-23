#pragma once

#include <string_view>

#include "mim/def.h"

namespace mim {

/// The machine a native backend compiles for, as far as libmim needs to know it.
/// A backend plugin registers its Target via Plugin::target; see Driver::target and mim::plug::mach::Target.
class Target {
public:
    Target(std::string_view name)
        : name_(name) {}
    virtual ~Target() = default;

    /// The plugin name, used as the prefix of diagnostics.
    std::string_view name() const { return name_; }
    /// The size and alignment of a pointer in bytes; see `core.trait`.
    virtual nat_t ptr_size() const = 0;

private:
    std::string_view name_;
};

} // namespace mim
