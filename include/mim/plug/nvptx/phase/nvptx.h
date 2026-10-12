#pragma once

#include <optional>
#include <string>

#include <mim/plug/ll/ll.h>

namespace mim {

class World;

namespace plug::nvptx {

/// Prefix for this backend's fe::throwf messages; see MIM_LL_BE.
#define MIM_NVPTX_BE "nvptx backend: "

namespace ll = mim::plug::ll;

struct DeviceEmitFlags {
    bool uses_libdevice;
};

void emit_host(World&, std::ostream&, std::optional<std::string>, ll::Emitter::Rt rt = ll::Emitter::Rt::embed);
DeviceEmitFlags emit_device(World&, std::ostream&);

} // namespace plug::nvptx

} // namespace mim
