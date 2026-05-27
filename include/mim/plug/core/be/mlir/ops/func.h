#pragma once
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class FuncReturnOp : public MLIROp {
public:
    // void return
    FuncReturnOp()
        : MLIROp({}, {}) {}

    // single value return
    FuncReturnOp(MLIRValue val)
        : MLIROp({}, {std::move(val)}) {}

    // multiple value return
    FuncReturnOp(std::vector<MLIRValue> vals)
        : MLIROp({}, std::move(vals)) {}

    void print(Printer& p) const override {
        if (operands_.empty()) {
            p.line("func.return");
            return;
        }
        std::string vals, types;
        for (auto& v : operands_) {
            vals += (vals.empty() ? "" : ", ") + v.name;
            types += (types.empty() ? "" : ", ") + print_type(v.type);
        }
        p.line("func.return {} : {}", vals, types);
    }
};

class FuncOp : public MLIROp {
public:
    FuncOp(std::string name, std::vector<MLIRValue> args, std::vector<MLIRType> ret_types, bool is_public = true)
        : MLIROp({}, {})
        , name_(std::move(name))
        , ret_types_(std::move(ret_types))
        , is_public_(is_public) {
        // function arguments become block args of the entry block
        body_.entry().args = std::move(args);
    }

    // the emitter populates the body through this
    MLIRRegion& body() { return body_; }

    void print(Printer& p) const override {
        // build param string from entry block args
        std::string params;
        for (auto& a : body_.blocks.front().args)
            params += (params.empty() ? "" : ", ") + a.name + ": " + print_type(a.type);

        // build return type string
        std::string ret;
        for (auto& t : ret_types_)
            ret += (ret.empty() ? "" : ", ") + print_type(t);

        auto linkage = is_public_ ? "public" : "private";

        if (ret.empty())
            p.line("func.func {} @{}({}) {{", linkage, name_, params);
        else
            p.line("func.func {} @{}({}) -> {} {{", linkage, name_, params, ret);

        p.indent();
        p.print_region(body_);
        p.dedent();
        p.line("}}");
    }

private:
    std::string name_;
    std::vector<MLIRType> ret_types_;
    bool is_public_;
    MLIRRegion body_;
};

} // namespace mim::mlir_be
