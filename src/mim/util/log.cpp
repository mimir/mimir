#include "mim/util/log.h"

namespace mim {

// clang-format off
char Log::level2acro(Level level) {
    switch (level) {
        case Level::Trace:   return 'T';
        case Level::Debug:   return 'D';
        case Level::Verbose: return 'V';
        case Level::Info:    return 'I';
        case Level::Warn:    return 'W';
        case Level::Error:   return 'E';
        default: fe::unreachable();
    }
}

fe::term::FG Log::level2color(Level level) {
    switch (level) {
        case Level::Trace:   return fe::term::FG::Magenta;
        case Level::Debug:   return fe::term::FG::Cyan;
        case Level::Verbose: return fe::term::FG::Blue;
        case Level::Info:    return fe::term::FG::Green;
        case Level::Warn:    return fe::term::FG::Yellow;
        case Level::Error:   return fe::term::FG::Red;
        default: fe::unreachable();
    }
}
// clang-format on

} // namespace mim
