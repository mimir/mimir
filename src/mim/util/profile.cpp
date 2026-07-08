#include "mim/util/profile.h"

#include <algorithm>
#include <print>

#include <absl/container/flat_hash_map.h>

namespace mim {

namespace {
double ms(Profiler::Duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}

double us(Profiler::Duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(d).count();
}

/// Escapes @p str for inclusion in a JSON string literal.
std::string json_escape(std::string_view str) {
    std::string res;
    for (auto c : str) {
        switch (c) {
            case '"': res += "\\\""; break;
            case '\\': res += "\\\\"; break;
            case '\n': res += "\\n"; break;
            case '\t': res += "\\t"; break;
            case '\r': res += "\\r"; break;
            default: res += c;
        }
    }
    return res;
}
} // namespace

Vector<Profiler::Duration> Profiler::children_durations() const {
    auto children = Vector<Duration>(spans_.size(), Duration::zero());
    for (const auto& span : spans_)
        if (span.parent != No_Parent) children[span.parent] += span.elapsed();
    return children;
}

void Profiler::summary(std::ostream& os) const {
    struct Agg {
        Duration total = Duration::zero();
        Duration self  = Duration::zero();
        size_t count   = 0;
    };

    auto children = children_durations();
    auto by_name  = absl::flat_hash_map<std::string_view, Agg>();
    auto total    = Duration::zero();

    for (size_t i = 0, e = spans_.size(); i != e; ++i) {
        auto self = spans_[i].elapsed() - children[i];
        auto& agg = by_name[spans_[i].name];
        agg.total += spans_[i].elapsed();
        agg.self += self;
        ++agg.count;
        total += self;
    }

    auto ordered = Vector<std::pair<std::string_view, Agg>>(by_name.begin(), by_name.end());
    std::ranges::sort(ordered, [](const auto& a, const auto& b) { return a.second.total > b.second.total; });

    std::println(os, "Phase profile (flat):");
    std::println(os, "{:>12}  {:>12}  {:>7}  {:>6}  {}", "total[ms]", "self[ms]", "self[%]", "#runs", "phase");
    for (const auto& [name, agg] : ordered) {
        auto percent = total > Duration::zero() ? 100.0 * ms(agg.self) / ms(total) : 0.0;
        std::println(os, "{:>12.3f}  {:>12.3f}  {:>6.1f}%  {:>6}  {}", ms(agg.total), ms(agg.self), percent, agg.count,
                     name);
    }
    std::println(os, "{:>12.3f}  {:>12.3f}  {:>6.1f}%  {:>6}  {}", ms(total), ms(total), 100.0, spans_.size(), "TOTAL");
}

void Profiler::tree(std::ostream& os) const {
    auto children = children_durations();
    auto total    = Duration::zero();
    for (const auto& span : spans_)
        if (span.parent == No_Parent) total += span.elapsed();

    std::println(os, "Phase profile (tree):");
    std::println(os, "{:>12}  {:>12}  {:>7}  {}", "total[ms]", "self[ms]", "tot[%]", "phase");
    for (size_t i = 0, e = spans_.size(); i != e; ++i) {
        const auto& span = spans_[i];
        auto self        = span.elapsed() - children[i];
        auto percent     = total > Duration::zero() ? 100.0 * ms(span.elapsed()) / ms(total) : 0.0;
        std::println(os, "{:>12.3f}  {:>12.3f}  {:>6.1f}%  {:>{}}{}", ms(span.elapsed()), ms(self), percent, "",
                     span.depth * 2, span.name);
    }
    std::println(os, "{:>12.3f}  {:>12}  {:>6.1f}%  {}", ms(total), "", 100.0, "TOTAL");
}

void Profiler::chrome_trace(std::ostream& os) const {
    auto origin = spans_.empty() ? Clock::time_point{} : spans_.front().start;

    std::println(os, "{{\"displayTimeUnit\":\"ms\",\"traceEvents\":[");
    for (size_t i = 0, e = spans_.size(); i != e; ++i) {
        const auto& span = spans_[i];
        std::println(os, "{{\"name\":\"{}\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":1,\"tid\":1,\"ts\":{:.3f},\"dur\":{:.3f}}}{}",
                     json_escape(span.name), us(span.start - origin), us(span.elapsed()), i + 1 == e ? "" : ",");
    }
    std::println(os, "]}}");
}

} // namespace mim
