#pragma once

#include <optional>
#include <string>

#include <mim/plug/ll/ll.h>

namespace mim {

class World;

namespace plug::ll_nvptx {

/// Prefix for this backend's fe::throwf messages; see MIM_LL_BE.
#define MIM_LL_NVPTX_BE "ll_nvptx backend: "

namespace ll = mim::plug::ll;

struct DeviceEmitFlags {
    bool uses_libdevice;
};

void emit_host(World&, std::ostream&, std::optional<std::string>, ll::Emitter::Rt rt = ll::Emitter::Rt::embed);
DeviceEmitFlags emit_device(World&, std::ostream&);

} // namespace plug::ll_nvptx

} // namespace mim
