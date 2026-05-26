#pragma once
#include <ostream>

#include "region_tree.h"

namespace mim::mlir_be {

struct Printer {
    std::ostream& os;
    int indent_level = 0;

    void indent() { ++indent_level; }
    void dedent() { --indent_level; }

    void print_region(const MLIRRegion& r);
    void print_block(const MLIRBlock& b);

    template<class... Args>
    void line(std::format_string<Args...> s, Args&&... args) {
        for (int i = 0; i < indent_level * 2; ++i)
            os << ' ';
        std::print(os, s, std::forward<Args>(args)...);
        os << '\n';
    }
};

} // namespace mim::mlir_be
