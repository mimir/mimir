#include <mlir/Dialect/Linalg/IR/Linalg.h>
#include <mlir/Dialect/Tensor/IR/Tensor.h>
#include <mlir/IR/MLIRContext.h>

#include "mim/plug/ascend/ascend.h"

#include "emit.h"
#include "hacc_dialect.h"

namespace mim::plug::ascend {

namespace {

mlir::linalg::BinaryFn binary_fn(BinOp op) {
    switch (op) {
        case BinOp::add: return mlir::linalg::BinaryFn::add;
        case BinOp::sub: return mlir::linalg::BinaryFn::sub;
        case BinOp::mul: return mlir::linalg::BinaryFn::mul;
        case BinOp::div: return mlir::linalg::BinaryFn::div;
    }
    llvm_unreachable("binary operation");
}

class EmitHFusion : public EmitPhase {
public:
    using EmitPhase::EmitPhase;

private:
    Built build(const Kernel& kernel) override {
        auto ctx = mlir::MLIRContext();
        ctx.loadDialect<mlir::func::FuncDialect, mlir::linalg::LinalgDialect, mlir::tensor::TensorDialect,
                        mlir::hacc::HACCDialect>();

        auto builder = mlir::OpBuilder(&ctx);
        auto loc     = builder.getUnknownLoc();
        auto module  = mlir::ModuleOp::create(loc);
        builder.setInsertionPointToEnd(module.getBody());

        auto f32    = builder.getF32Type();
        auto tensor = mlir::RankedTensorType::get(dims(kernel), f32);
        auto params = mlir::SmallVector<mlir::Type>(kernel.num_params, tensor);
        auto func   = create_entry(builder, kernel, builder.getFunctionType(params, tensor));
        builder.setInsertionPointToStart(func.addEntryBlock());

        auto values = mlir::SmallVector<mlir::Value>(func.getArguments());
        values.resize(kernel.num_values());

        for (size_t i = 0, e = kernel.nodes.size(); i != e; ++i) {
            auto& node = kernel.nodes[i];
            auto empty = builder.create<mlir::tensor::EmptyOp>(loc, dims(kernel), f32);
            auto fun   = mlir::linalg::BinaryFnAttr::get(&ctx, binary_fn(node.op));
            auto elem  = builder.create<mlir::linalg::ElemwiseBinaryOp>(
                loc, mlir::TypeRange{tensor}, mlir::ValueRange{values[node.lhs], values[node.rhs]},
                mlir::ValueRange{empty}, mlir::ArrayRef{builder.getNamedAttr("fun", fun)});
            values[kernel.node_id(i)] = elem.getResult(0);
        }

        builder.create<mlir::func::ReturnOp>(loc, values[kernel.result]);
        return print_verified(module);
    }
};

} // namespace

void register_hfusion(Flags2Phases& phases) { Phase::hook<ascend::emit_hfusion, EmitHFusion>(phases); }

} // namespace mim::plug::ascend
