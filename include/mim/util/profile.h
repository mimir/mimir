#pragma once

#include <chrono>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>

#include "mim/util/vector.h"

namespace mim {

/// Records wall-clock timings for (possibly nested) Phase runs and reports them in various formats.
/// Phase%es nest: a PhaseMan runs sub-Phase%es, so each run is recorded as a Profiler::Span that remembers its parent.
/// From these Span%s the Profiler can derive
/// - a flat Profiler::summary aggregated by Phase name,
/// - a hierarchical Profiler::tree that shows the runtimes in context, and
/// - a [Chrome Trace Event](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU)
///   JSON dump (Profiler::chrome_trace) that can be loaded into `chrome://tracing`, Perfetto, or speedscope.
/// @see Profiler::start, Profiler::stop
class Profiler {
public:
    using Clock    = std::chrono::steady_clock;
    using Duration = Clock::duration;

    static constexpr size_t No_Parent = std::numeric_limits<size_t>::max();

    /// A single Phase run.
    struct Span {
        std::string name;
        Clock::time_point start;
        Clock::time_point stop;
        size_t depth;  ///< Nesting level; a root Phase has depth 0.
        size_t parent; ///< Index of the enclosing Span in Profiler::spans, or Profiler::No_Parent for a root.

        Duration elapsed() const { return stop - start; }
    };

    /// @name Getters
    ///@{
    bool empty() const { return spans_.empty(); }
    const auto& spans() const { return spans_; }
    ///@}

    /// @name Recording
    /// Bracket a Phase run with Profiler::start / Profiler::stop; the calls must nest like a stack.
    ///@{
    /// Marks the start of a Phase run named @p name.
    void start(std::string_view name) {
        auto parent = stack_.empty() ? No_Parent : stack_.back();
        stack_.emplace_back(spans_.size());
        spans_.emplace_back(std::string(name), Clock::now(), Clock::time_point{}, stack_.size() - 1, parent);
    }

    /// Marks the end of the most recently started Phase run.
    void stop() {
        auto id = stack_.back();
        stack_.pop_back();
        spans_[id].stop = Clock::now();
    }
    ///@}

    /// @name Reporting
    ///@{
    /// Prints a flat table aggregated by Phase name, sorted by total time, descending.
    void summary(std::ostream&) const;
    /// Prints the Span%s as an indented tree, preserving the order in which Phase%es ran.
    void tree(std::ostream&) const;
    /// Dumps all Span%s as Chrome Trace Event Format JSON.
    void chrome_trace(std::ostream&) const;
    ///@}

private:
    /// Per-Span time spent in *direct* children; `self = elapsed - children`.
    Vector<Duration> children_durations() const;

    Vector<size_t> stack_;
    Vector<Span> spans_;
};

} // namespace mim
