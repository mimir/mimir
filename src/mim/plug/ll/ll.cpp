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
/// Writes the LLVM IR of the fully lowered world to `<world>.ll` or `a.ll`.
class Emit : public Phase {
public:
    Emit(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name    = world().name() ? std::string(world().name().view()) : "a"s;
        auto ofs     = std::ofstream(name + ".ll"s);
        auto emitter = Emitter(world(), ofs);
        emitter.run();
    }
};

} // namespace mim::plug::ll

using namespace mim;

static void reg_stages(Flags2Stages& stages) { Stage::hook<plug::ll::emit, plug::ll::Emit>(stages); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"ll", MIM_VERSION, nullptr, reg_stages}; }
