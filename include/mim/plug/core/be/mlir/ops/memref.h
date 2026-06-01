#pragma once
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class TensorEmptyOp : public MLIROp {
public:
    TensorEmptyOp(MLIRValue result)
        : MLIROp({std::move(result)}, {}) {}

    void print(Printer& p) const override {
        p.line("{} = tensor.empty() : {}", results_[0].name, print_type(results_[0].type));
    }
};

} // namespace mim::mlir_be
