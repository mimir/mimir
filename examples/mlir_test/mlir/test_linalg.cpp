#include <cassert>

#include <fstream>
#include <iostream>
#include <sstream>

#include "ops/arith.h"
#include "ops/func.h"
#include "ops/linalg.h"
#include "region_tree.h"

using namespace mim::mlir_be;

int main(int argc, char** argv) {
    MLIRType f64{MLIRFloatType{64}};

    auto make_tensor = [](std::vector<std::optional<int64_t>> shape, MLIRType elem) -> MLIRType {
        MLIRTensorType t;
        t.shape = std::move(shape);
        t.elem  = std::make_shared<MLIRTypeNode>(std::move(elem));
        return MLIRType{std::move(t)};
    };

    MLIRType tA = make_tensor({4, 8}, f64);
    MLIRType tB = make_tensor({8, 4}, f64);
    MLIRType tC = make_tensor({4, 4}, f64);

    // build linalg.generic
    LinalgGenericOp matmul{
        {MLIRValue{"%A", tA}, MLIRValue{"%B", tB}}, // ins
        {MLIRValue{"%C", tC}}, // outs
        {"affine_map<(d0, d1, d2) -> (d0, d2)>", "affine_map<(d0, d1, d2) -> (d2, d1)>",
         "affine_map<(d0, d1, d2) -> (d0, d1)>"},
        {"parallel", "parallel", "reduction"},
        {MLIRValue{"%a", f64}, MLIRValue{"%b", f64}, MLIRValue{"%c", f64}}
    };

    // scalar body
    auto& bb = matmul.body().entry();
    auto mul = bb.push<BinaryFloatOp>(MLIRValue{"%mul", f64}, BinaryFloatOp::Kind::Mul, MLIRValue{"%a", f64},
                                      MLIRValue{"%b", f64});
    auto add = bb.push<BinaryFloatOp>(MLIRValue{"%add", f64}, BinaryFloatOp::Kind::Add, mul, MLIRValue{"%c", f64});
    bb.push_void<LinalgYieldOp>(std::vector<MLIRValue>{add});

    // wrap in func.func
    FuncOp func{
        "matmul",
        {MLIRValue{"%A", tA}, MLIRValue{"%B", tB}, MLIRValue{"%C", tC}},
        {tC}
    };
    func.body().entry().push_void<LinalgGenericOp>(std::move(matmul));
    func.body().entry().push_void<FuncReturnOp>(std::vector<MLIRValue>{
        MLIRValue{"%C.out", tC}
    });

    // wrap in module
    MLIRRegion module;
    module.entry().push_void<FuncOp>(std::move(func));

    // print
    std::ostringstream os;
    Printer p{os};
    p.print_region(module);
    auto actual = os.str();

    std::cout << actual;

    // optionally dump to file
    if (argc > 1) {
        std::ofstream f(argv[1]);
        f << actual;
        std::cout << "written to " << argv[1] << "\n";
    }

    return 0;
}
