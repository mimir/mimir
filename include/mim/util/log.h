#pragma once

#include <ostream>
#include <print>

#include <fe/format.h>
#include <fe/term.h>

#include "mim/flags.h"

#include "mim/util/dbg.h"

namespace mim {

namespace fs = std::filesystem;

/// Facility to log what you are doing.
/// @see @ref log "Logging Macros"
class Log {
public:
    Log(const Flags& flags)
        : flags_(flags) {}

    enum class Level { Error, Warn, Info, Verbose, Debug, Trace };

    /// @name Getters
    ///@{
    const Flags& flags() const { return flags_; }
    Level level() const { return max_level_; }
    std::ostream& ostream() const {
        assert(ostream_);
        return *ostream_;
    }
    explicit operator bool() const { return ostream_; } ///< Checks if Log::ostream_ is set.
    ///@}

    /// @name Setters
    ///@{
    Log& set(std::ostream* ostream) {
        ostream_ = ostream;
        return *this;
    }
    Log& set(Level max_level) {
        max_level_ = max_level;
        return *this;
    }
    ///@}

    /// @name Log
    /// Output @p fmt to Log::ostream; does nothing if Log::ostream is `nullptr`.
    /// @see @ref log "Logging Macros"
    ///@{
    template<class... Args>
    void log(Level level, Loc loc, std::format_string<Args...> fmt, Args&&... args) const {
        if (ostream_ && level <= max_level_) emit(level, loc, fmt, std::forward<Args>(args)...);
    }

    /// A `__FILE__`/`__LINE__` pair is no Loc: it points into *our* source, which has no fe::Src.
    template<class... Args>
    void log(Level level, const char* file, uint32_t line, std::format_string<Args...> fmt, Args&&... args) const {
        if (ostream_ && level <= max_level_)
            emit(level, std::format("{}:{}", file, line), fmt, std::forward<Args>(args)...);
    }
    ///@}

    /// @name Conversions
    ///@{
    static char level2acro(Level);
    static fe::term::FG level2color(Level level);
    ///@}

private:
    template<class W, class... Args>
    void emit(Level level, const W& where, std::format_string<Args...> fmt, Args&&... args) const {
        std::print(ostream(), "{}{}:{}{}:{} ", level2color(level), level2acro(level), fe::term::FG::Gray, where,
                   fe::term::FG::Reset);
        std::println(ostream(), fmt, std::forward<Args>(args)...);
#ifdef MIM_ENABLE_CHECKS
        if ((level == Level::Error && flags().break_on_error) || (level == Level::Warn && flags().break_on_warn))
            fe::breakpoint();
#endif
    }

    const Flags& flags_;
    std::ostream* ostream_ = nullptr;
    Level max_level_       = Level::Error;
};

/// @name Logging Macros
/// @anchor log
/// Macros for different mim::Log::Level%s for ease of use.
///@{
// clang-format off
#define ELOG(...) log().log(mim::Log::Level::Error,   __FILE__, __LINE__, __VA_ARGS__)
#define WLOG(...) log().log(mim::Log::Level::Warn,    __FILE__, __LINE__, __VA_ARGS__)
#define ILOG(...) log().log(mim::Log::Level::Info,    __FILE__, __LINE__, __VA_ARGS__)
#define VLOG(...) log().log(mim::Log::Level::Verbose, __FILE__, __LINE__, __VA_ARGS__)
/// Vaporizes to nothingness in `Debug` build.
#ifndef NDEBUG
#define DLOG(...) log().log(mim::Log::Level::Debug,   __FILE__, __LINE__, __VA_ARGS__)
#define TLOG(...) log().log(mim::Log::Level::Trace,   __FILE__, __LINE__, __VA_ARGS__)
#else
#define DLOG(...) log()
#define TLOG(...) log()
#endif
// clang-format on
///@}

} // namespace mim
