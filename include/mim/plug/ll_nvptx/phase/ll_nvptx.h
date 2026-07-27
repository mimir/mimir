#pragma once

#include <optional>
#include <string>

#include <mim/plug/ll/ll.h>

namespace mim {

class World;

namespace plug::ll_nvptx {

struct DeviceEmitFlags {
    bool uses_libdevice;
};

void emit_host(World&, std::ostream&, std::optional<std::string>);
DeviceEmitFlags emit_device(World&, std::ostream&);

} // namespace plug::ll_nvptx

} // namespace mim
