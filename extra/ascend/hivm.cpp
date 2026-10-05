#include <charconv>

#include <limits>

#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/IR/MLIRContext.h>

#include <mim/plugin.h>

#include "mim/plug/ascend/ascend.h"

#include "emit.h"
#include "hacc_dialect.h"
#include "hivm_dialect.h"

namespace mim::plug::ascend {

namespace {

/// UB_SIZE of Ascend950PR_9579 (dav-c310), read from the bishengir target spec.
constexpr uint64_t ascend950_ub_bytes = 2031616;

constexpr uint64_t f32_bytes = 4;

/// Parameters the kernel actually reads, in ascending order; each one needs its own GM to UB transfer.
fe::Vector<size_t> loaded_params(const Kernel& kernel) {
    auto loaded = fe::Vector<bool>(kernel.num_params, false);
    for (const auto& node : kernel.nodes) {
        if (kernel.is_param(node.lhs)) loaded[node.lhs] = true;
        if (kernel.is_param(node.rhs)) loaded[node.rhs] = true;
    }
    if (kernel.is_param(kernel.result)) loaded[kernel.result] = true;

    auto params = fe::Vector<size_t>();
    for (size_t i = 0; i != kernel.num_params; ++i)
        if (loaded[i]) params.emplace_back(i);
    return params;
}

/// Every loaded parameter and every intermediate result is one `memref.alloc` in the unified buffer.
uint64_t ub_bytes(const Kernel& kernel) {
    auto elems   = kernel.num_elems();
    auto buffers = static_cast<uint64_t>(loaded_params(kernel).size() + kernel.nodes.size());
    if (elems == 0 || buffers == 0) return 0;
    if (buffers > std::numeric_limits<uint64_t>::max() / elems / f32_bytes)
        fe::throwf("ascend backend tensor is too large for the unified buffer");
    return elems * f32_bytes * buffers;
}

uint64_t parse_ub(std::string_view raw) {
    auto value     = uint64_t{0};
    auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || value == 0)
        fe::throwf("ascend backend expected a positive byte count for `ub`, but got `{}`", raw);
    return value;
}

void create_binary(mlir::OpBuilder& builder,
                   mlir::Location loc,
                   BinOp op,
                   mlir::Value lhs,
                   mlir::Value rhs,
                   mlir::Value dst) {
    switch (op) {
        case BinOp::add: builder.create<mlir::hivm::VAddOp>(loc, lhs, rhs, dst); break;
        case BinOp::sub: builder.create<mlir::hivm::VSubOp>(loc, lhs, rhs, dst); break;
        case BinOp::mul: builder.create<mlir::hivm::VMulOp>(loc, lhs, rhs, dst); break;
        case BinOp::div: builder.create<mlir::hivm::VDivOp>(loc, lhs, rhs, dst); break;
    }
}

class EmitHIVM : public EmitPhase {
public:
    using EmitPhase::EmitPhase;

private:
    void check(const Kernel& kernel) override {
        auto capacity = ascend950_ub_bytes;
        if (auto raw = arg_value(args(), "ub")) capacity = parse_ub(*raw);

        if (auto bytes = ub_bytes(kernel); bytes > capacity)
            fe::throwf("ascend backend requires the kernel to fit in one unified-buffer allocation ({} bytes), but "
                       "this kernel needs {} bytes",
                       capacity, bytes);
    }

    Built build(const Kernel& kernel) override {
        auto ctx = mlir::MLIRContext();
        ctx.loadDialect<mlir::func::FuncDialect, mlir::memref::MemRefDialect, mlir::hivm::HIVMDialect,
                        mlir::hacc::HACCDialect>();

        auto builder = mlir::OpBuilder(&ctx);
        auto loc     = builder.getUnknownLoc();
        auto module  = mlir::ModuleOp::create(loc);
        builder.setInsertionPointToEnd(module.getBody());

        auto memref = [&](mlir::hivm::AddressSpace space) {
            return mlir::MemRefType::get(dims(kernel), builder.getF32Type(), mlir::AffineMap(),
                                         mlir::hivm::AddressSpaceAttr::get(&ctx, space));
        };
        auto gm = memref(mlir::hivm::AddressSpace::GM);
        auto ub = memref(mlir::hivm::AddressSpace::UB);

        // The MimIR result becomes one extra global-memory argument that the kernel writes.
        auto params = mlir::SmallVector<mlir::Type>(kernel.num_params + 1, gm);
        auto func   = create_entry(builder, kernel, builder.getFunctionType(params, {}));
        builder.setInsertionPointToStart(func.addEntryBlock());

        auto args   = func.getArguments();
        auto values = mlir::SmallVector<mlir::Value>(kernel.num_values());

        for (auto param : loaded_params(kernel)) {
            auto alloc = builder.create<mlir::memref::AllocOp>(loc, ub);
            builder.create<mlir::hivm::LoadOp>(loc, args[param], alloc);
            values[param] = alloc;
        }

        for (size_t i = 0, e = kernel.nodes.size(); i != e; ++i) {
            auto& node = kernel.nodes[i];
            auto alloc = builder.create<mlir::memref::AllocOp>(loc, ub);
            create_binary(builder, loc, node.op, values[node.lhs], values[node.rhs], alloc);
            values[kernel.node_id(i)] = alloc;
        }

        builder.create<mlir::hivm::StoreOp>(loc, values[kernel.result], args.back());
        builder.create<mlir::func::ReturnOp>(loc);
        return print_verified(module);
    }
};

} // namespace

void register_hivm(Flags2Phases& phases) { Phase::hook<ascend::emit_hivm, EmitHIVM>(phases); }

} // namespace mim::plug::ascend
