#pragma once
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class LinalgYieldOp : public MLIROp {
public:
    explicit LinalgYieldOp(std::vector<MLIRValue> vals)
        : MLIROp({}, std::move(vals)) {}

    void print(Printer& p) const override {
        if (operands_.empty()) {
            p.line("linalg.yield");
            return;
        }
        std::string vals, types;
        for (auto& v : operands_) {
            vals += (vals.empty() ? "" : ", ") + v.name;
            types += (types.empty() ? "" : ", ") + print_type(v.type);
        }
        p.line("linalg.yield {} : {}", vals, types);
    }
};

class LinalgGenericOp : public MLIROp {
public:
    LinalgGenericOp(std::vector<MLIRValue> ins,
                    std::vector<MLIRValue> outs,
                    std::vector<std::string> indexing_maps,
                    std::vector<std::string> iterator_types,
                    std::vector<MLIRValue> body_args // scalar element types
                    )
        : MLIROp(
              // results mirror the outs types
              [&] {
                  std::vector<MLIRValue> res;
                  for (auto& o : outs)
                      res.push_back({"%" + o.name.substr(1) + ".out", o.type});
                  return res;
              }(),
              {})
        , ins_(std::move(ins))
        , outs_(std::move(outs))
        , indexing_maps_(std::move(indexing_maps))
        , iterator_types_(std::move(iterator_types)) {
        // body block args are the scalar element types
        for (auto& a : body_args)
            body_.entry().args.push_back(a);
    }

    MLIRRegion& body() { return body_; }

    void print(Printer& p) const override {
        // result lhs
        std::string lhs;
        for (auto& r : results_)
            lhs += (lhs.empty() ? "" : ", ") + r.name;
        if (!lhs.empty()) lhs += " = ";

        // indexing maps on one line
        std::string maps;
        for (auto& m : indexing_maps_)
            maps += (maps.empty() ? "" : ", ") + m;

        // iterator types
        std::string iters;
        for (auto& t : iterator_types_)
            iters += (iters.empty() ? "" : ", ") + ("\"" + t + "\"");

        // attributes on one line, then ins/outs
        p.line("{}linalg.generic {{indexing_maps = [{}], iterator_types = [{}]}}", lhs, maps, iters);

        // ins
        std::string ins_names, ins_types;
        for (auto& v : ins_) {
            ins_names += (ins_names.empty() ? "" : ", ") + v.name;
            ins_types += (ins_types.empty() ? "" : ", ") + print_type(v.type);
        }

        // outs
        std::string out_names, out_types;
        for (auto& v : outs_) {
            out_names += (out_names.empty() ? "" : ", ") + v.name;
            out_types += (out_types.empty() ? "" : ", ") + print_type(v.type);
        }

        p.indent();
        p.line("ins({} : {})", ins_names, ins_types);
        p.line("outs({} : {}) {{", out_names, out_types);

        // body block with explicit ^bb0 label and args
        p.indent();
        std::string block_args;
        for (auto& a : body_.entry().args)
            block_args += (block_args.empty() ? "" : ", ") + a.name + ": " + print_type(a.type);
        p.line("^bb0({}):", block_args);

        p.indent();
        for (auto& op : body_.entry().ops)
            op->print(p);
        p.dedent();

        p.dedent();
        p.line("}}");

        // result types after closing brace
        std::string ret_types;
        for (auto& r : results_)
            ret_types += (ret_types.empty() ? "" : ", ") + print_type(r.type);
        p.dedent();
        p.line("-> {}", ret_types);
    }

private:
    std::vector<MLIRValue> ins_;
    std::vector<MLIRValue> outs_;
    std::vector<std::string> indexing_maps_;
    std::vector<std::string> iterator_types_;
    MLIRRegion body_;
};

class LinalgBroadcastOp : public MLIROp {
public:
    LinalgBroadcastOp(MLIRValue result, MLIRValue input, MLIRValue out_buf, std::vector<int64_t> broadcast_dims)
        : MLIROp({std::move(result)}, {std::move(input), std::move(out_buf)})
        , broadcast_dims_(std::move(broadcast_dims)) {}

    void print(Printer& p) const override {
        // dimensions string: [0] or [0, 2] etc.
        std::string dims;
        for (size_t i = 0; i < broadcast_dims_.size(); ++i)
            dims += (i ? ", " : "") + std::to_string(broadcast_dims_[i]);

        p.line("{} = linalg.broadcast ins({} : {})", results_[0].name, operands_[0].name,
               print_type(operands_[0].type));
        p.indent();
        p.line("outs({} : {})", operands_[1].name, print_type(operands_[1].type));
        p.line("dimensions = [{}]", dims);
        p.dedent();
    }

private:
    std::vector<int64_t> broadcast_dims_;
};

} // namespace mim::mlir_be
