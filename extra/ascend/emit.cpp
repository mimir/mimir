#include "emit.h"

#include <mlir/IR/Diagnostics.h>

#include <mim/plugin.h>

#include "hacc_dialect.h"

namespace mim::plug::ascend {

mlir::func::FuncOp create_entry(mlir::OpBuilder& builder, const Kernel& kernel, mlir::FunctionType type) {
    auto* ctx = builder.getContext();
    auto func = builder.create<mlir::func::FuncOp>(builder.getUnknownLoc(), kernel.name, type);
    func->setAttr("hacc.entry", mlir::UnitAttr::get(ctx));
    func->setAttr("hacc.function_kind", mlir::hacc::HACCFuncTypeAttr::get(ctx, mlir::hacc::HACCFuncType::DEVICE));
    return func;
}

Built print_verified(mlir::ModuleOp module) {
    auto error   = std::string();
    auto handler = mlir::ScopedDiagnosticHandler(module.getContext(), [&](mlir::Diagnostic& diag) {
        auto os = llvm::raw_string_ostream(error);
        os << diag << '\n';
        return mlir::success();
    });

    if (failed(module.verify())) {
        if (error.empty()) error = "verification failed\n";
        return {{}, std::move(error)};
    }

    auto text = std::string();
    auto os   = llvm::raw_string_ostream(text);
    module.print(os);
    return {std::move(text), {}};
}

void EmitPhase::start() {
    auto kernel = analyze(world());
    check(kernel);

    auto built = build(kernel);
    if (!built.error.empty()) fe::throwf("ascend backend generated MLIR that failed verification: {}", built.error);

    auto name = world().name() ? world().name().str() : std::string("a");
    auto path = name + ".ascend.mlir";
    if (auto output = arg_value(args(), "o", "output")) path = *output;

    auto out = Out(std::move(path));
    *out.os() << built.text;
}

} // namespace mim::plug::ascend
