#include "mim/plug/ll/ll.h"

#include <fstream>
#include <string>

#include <mim/config.h>
#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/ll/autogen.h"

namespace mim::plug::ll {

using namespace std::string_literals;

/// Pipeline phase for `%ll.emit`.
/// Writes the LLVM IR of the fully lowered world to `<world>.ll` (or `a.ll` if the world is unnamed).
/// The output path can be overridden on the command line via `-X ll:o=<file>`.
class Emit : public Phase {
public:
    Emit(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? std::string(world().name().view()) : "a"s;
        auto path = name + ".ll"s;
        for (const auto& arg : args()) {
            world().DLOG("ll backend arg: `{}`", arg);
            if (arg.starts_with("o=")) path = arg.substr(2);
        }
        auto ofs     = std::ofstream(path);
        auto emitter = Emitter(world(), ofs);
        emitter.run();
    }
};

} // namespace mim::plug::ll

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::ll::emit, plug::ll::Emit>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"ll", MIM_VERSION, nullptr, reg_phases}; }
