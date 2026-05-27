#pragma once
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class AffineYieldOp : public MLIROp {
public:
    AffineYieldOp()
        : MLIROp({}, {}) {}

    void print(Printer& p) const override { p.line("affine.yield"); }
};

class AffineForOp : public MLIROp {
public:
    AffineForOp(MLIRValue iv, int64_t lb, int64_t ub, int64_t step = 1)
        : MLIROp({}, {})
        , iv_(std::move(iv))
        , lb_(lb)
        , ub_(ub)
        , step_(step) {
        // IV becomes the block argument of the body
        body_.entry().args.push_back(iv_);
    }

    MLIRRegion& body() { return body_; }

    void print(Printer& p) const override {
        p.line("affine.for {} = {} to {} step {} {{", iv_.name, lb_, ub_, step_);
        p.indent();
        p.print_region(body_);
        p.dedent();
        p.line("}}");
    }

private:
    MLIRValue iv_;
    int64_t lb_, ub_, step_;
    MLIRRegion body_;
};

} // namespace mim::mlir_be
