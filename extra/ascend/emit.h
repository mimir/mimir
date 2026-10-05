#pragma once

#include <string>

#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>

#include <mim/phase.h>

#include "kernel.h"

namespace mim::plug::ascend {

/// The printed module, or the MLIR verifier's complaint about it.
struct Built {
    std::string text;
    std::string error;
};

inline llvm::ArrayRef<int64_t> dims(const Kernel& kernel) { return {kernel.shape.data(), kernel.shape.size()}; }

/// A `func.func` carrying the HACC attributes that mark an Ascend device entry point.
mlir::func::FuncOp create_entry(mlir::OpBuilder&, const Kernel&, mlir::FunctionType);

Built print_verified(mlir::ModuleOp);

/// Turns the World's kernel into one MLIR module and writes it to `-X ascend:o`.
class EmitPhase : public Phase {
public:
    EmitPhase(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() final;

private:
    /// Rejects a kernel this emitter cannot handle, before any MLIR object exists.
    virtual void check(const Kernel&) {}

    /// Must not throw: the MLIR libraries are compiled without exception unwind tables.
    virtual Built build(const Kernel&) = 0;
};

void register_hfusion(Flags2Phases&);
void register_hivm(Flags2Phases&);

} // namespace mim::plug::ascend
