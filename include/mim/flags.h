#pragma once

#include <cstdint>

#include "mim/config.h"

namespace mim {

/// Compiler switches that must be saved and looked up in later phases of compilation.
/// @see @ref cli
struct Flags {
    /// How (if at all) to report Phase runtimes; @see Profiler.
    enum class Profile {
        None,    ///< No profiling.
        Summary, ///< Flat table aggregated by Phase name.
        Tree,    ///< Indented tree preserving the order in which Phase%es ran.
        Trace,   ///< chrome://tracing compatible output.
    };

    uint64_t scalarize_threshold = 32;
    bool ascii                   = false;
    bool dump_recursive          = false;
    bool bootstrap               = false;
    bool force_load              = false;
    Profile profile              = Profile::None; // how to report Phase runtimes
    bool aggressive_lam_spec     = false;         // HACK makes LamSpec more agressive but potentially non-terminating
#ifdef MIM_ENABLE_CHECKS
    bool reeval_breakpoints = false;
    bool trace_gids         = false;
    bool break_on_error     = false;
    bool break_on_warn      = false;
    bool break_on_alpha     = false;
#endif
};

} // namespace mim
