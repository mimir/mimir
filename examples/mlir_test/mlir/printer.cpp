#include "printer.h"

namespace mim::mlir_be {

void Printer::print_region(const MLIRRegion& r) {
    for (auto& block : r.blocks)
        print_block(block);
}

void Printer::print_block(const MLIRBlock& b) {
    if (!b.label.empty()) {
        std::string args;
        for (auto& a : b.args)
            args += (args.empty() ? "" : ", ") + a.name + ": " + print_type(a.type);
        line("^{}({}):", b.label, args);
    }
    indent();
    for (auto& op : b.ops)
        op->print(*this);
    dedent();
}

} // namespace mim::mlir_be
