#pragma once
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class SCFYieldOp : public MLIROp {
public:
    SCFYieldOp()
        : MLIROp({}, {}) {}

    explicit SCFYieldOp(std::vector<MLIRValue> vals)
        : MLIROp({}, std::move(vals)) {}

    void print(Printer& p) const override {
        if (operands_.empty()) {
            p.line("scf.yield");
            return;
        }
        std::string vals, types;
        for (auto& v : operands_) {
            vals += (vals.empty() ? "" : ", ") + v.name;
            types += (types.empty() ? "" : ", ") + print_type(v.type);
        }
        p.line("scf.yield {} : {}", vals, types);
    }
};

class SCFForOp : public MLIROp {
public:
    SCFForOp(MLIRValue iv, MLIRValue lb, MLIRValue ub, MLIRValue step, std::vector<MLIRValue> iter_args = {})
        : MLIROp({}, {std::move(lb), std::move(ub), std::move(step)})
        , iv_(std::move(iv))
        , iter_args_(std::move(iter_args)) {
        // IV is the first block arg, iter args follow
        body_.entry().args.push_back(iv_);
        for (auto& a : iter_args_)
            body_.entry().args.push_back(a);
    }

    MLIRRegion& body() { return body_; }

    void print(Printer& p) const override {
        // iter args on the op line: scf.for %i = %lb to %ub step %step iter_args(%a = %init)
        std::string iter_str;
        for (auto& a : iter_args_)
            iter_str += (iter_str.empty() ? "" : ", ") + a.name + " = " + a.name;

        if (iter_str.empty())
            p.line("scf.for {} = {} to {} step {} {{", iv_.name, operands_[0].name, operands_[1].name,
                   operands_[2].name);
        else
            p.line("scf.for {} = {} to {} step {} iter_args({}) {{", iv_.name, operands_[0].name, operands_[1].name,
                   operands_[2].name, iter_str);

        p.indent();
        p.print_region(body_);
        p.dedent();
        p.line("}}");
    }

private:
    MLIRValue iv_;
    std::vector<MLIRValue> iter_args_;
    MLIRRegion body_;
};

} // namespace mim::mlir_be
