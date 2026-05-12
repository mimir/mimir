#pragma once

#include <cstdint>

#include <format>
#include <optional>
#include <string_view>
#include <vector>

namespace automaton {

using Range = std::pair<std::uint64_t, std::uint64_t>;

struct RangeCompare {
    inline bool operator()(const Range& a, const Range& b) const noexcept { return a.first < b.first; }
};

inline std::optional<Range> merge_ranges(Range a, Range b) noexcept {
    if (!(a.second + 1 < b.first || b.second + 1 < a.first)) {
        return {
            {std::min(a.first, b.first), std::max(a.second, b.second)}
        };
    }
    return {};
}

// precondition: ranges are sorted by increasing lower bound
template<class Vec, class LogF>
Vec merge_ranges(const Vec& old_ranges, LogF&& log) {
    Vec new_ranges;
    for (auto it = old_ranges.begin(); it != old_ranges.end(); ++it) {
        auto current_range = *it;
        log(std::format("old range: {}-{}", current_range.first, current_range.second));
        for (auto inner = it + 1; inner != old_ranges.end(); ++inner)
            if (auto merged = merge_ranges(current_range, *inner)) current_range = *merged;

        std::vector<typename Vec::iterator> de_duplicate;
        for (auto inner = new_ranges.begin(); inner != new_ranges.end(); ++inner) {
            if (auto merged = merge_ranges(current_range, *inner)) {
                current_range = *merged;
                de_duplicate.push_back(inner);
            }
        }
        for (auto dedup : de_duplicate) {
            log(std::format("dedup {}-{}", current_range.first, current_range.second));
            new_ranges.erase(dedup);
        }
        log(std::format("new range: {}-{}", current_range.first, current_range.second));
        new_ranges.push_back(std::move(current_range));
    }
    return new_ranges;
}

// precondition: ranges are sorted by increasing lower bound
template<class Vec>
auto merge_ranges(const Vec& old_ranges) {
    return merge_ranges(old_ranges, [](std::string_view) {});
}

} // namespace automaton
