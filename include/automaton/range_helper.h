#pragma once

#include <concepts>
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
template<class Vec>
Vec merge_ranges(const Vec& old_ranges, std::invocable<std::string_view> auto log) {
    Vec new_ranges;
    for (const auto& range : old_ranges) {
        log(std::format("old range: {}-{}", range.first, range.second));
        if (!new_ranges.empty())
            if (auto merged = merge_ranges(new_ranges.back(), range)) {
                new_ranges.back() = *merged;
                continue;
            }
        new_ranges.push_back(range);
    }
    return new_ranges;
}

// precondition: ranges are sorted by increasing lower bound
template<class Vec>
auto merge_ranges(const Vec& old_ranges) {
    return merge_ranges(old_ranges, [](std::string_view) {});
}

} // namespace automaton
