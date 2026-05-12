#pragma once

#include <cstdint>

#include "mim/config.h"

namespace mim {

/// Compiler switches that must be saved and looked up in later phases of compilation.
/// @see @ref cli
struct Flags {
    uint64_t scalarize_threshold = 32;
    bool ascii                   = false;
    bool dump_recursive          = false;
    bool bootstrap               = false;
    bool force_load              = false;
    bool aggressive_lam_spec     = false; // HACK makes LamSpec more agressive but potentially non-terminating
#ifdef MIM_ENABLE_CHECKS
    bool reeval_breakpoints = false;
    bool trace_gids         = false;
    bool break_on_error     = false;
    bool break_on_warn      = false;
    bool break_on_alpha     = false;
#endif
};

} // namespace mim
