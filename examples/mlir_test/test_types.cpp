#include <cassert>

#include <iostream>
#include <sstream>

#include "mim/plug/core/be/mlir/ops/affine.h"
#include "mim/plug/core/be/mlir/ops/arith.h"
#include "mim/plug/core/be/mlir/ops/func.h"
#include "mim/plug/core/be/mlir/ops/linalg.h"
#include "mim/plug/core/be/mlir/ops/scf.h"
#include "mim/plug/core/be/mlir/region_tree.h"

using namespace mim::mlir_be;

int main() {
    // Scalar types — wrap in MLIRType explicitly to avoid ambiguity
    assert(print_type(MLIRType{MLIRIntType{32}}) == "i32");
    assert(print_type(MLIRType{MLIRIntType{1}}) == "i1");
    assert(print_type(MLIRType{MLIRIndexType{}}) == "index");
    assert(print_type(MLIRType{MLIRFloatType{32}}) == "f32");
    assert(print_type(MLIRType{MLIRFloatType{64}}) == "f64");

    // tensor<4x8xf32>
    MLIRTensorType t;
    t.shape = {4, 8};
    t.elem  = std::make_shared<MLIRTypeNode>(MLIRFloatType{32});
    assert(print_type(MLIRType{std::move(t)}) == "tensor<4x8xf32>");

    // memref<?xi32>
    MLIRMemrefType m;
    m.shape = {std::nullopt};
    m.elem  = std::make_shared<MLIRTypeNode>(MLIRIntType{32});
    assert(print_type(MLIRType{std::move(m)}) == "memref<?xi32>");

    // (i32, f64) -> (index)
    MLIRFuncType f;
    f.inputs.emplace_back(MLIRIntType{32});
    f.inputs.emplace_back(MLIRFloatType{64});
    f.results.emplace_back(MLIRIndexType{});
    assert(print_type(MLIRType{std::move(f)}) == "(i32, f64) -> (index)");

    // Attrs
    {
        assert(print_attr(IntAttr{42, MLIRType{MLIRIntType{32}}}) == "42 : i32");
        assert(print_attr(IntAttr{-1, MLIRType{MLIRIntType{64}}}) == "-1 : i64");
        assert(print_attr(FloatAttr{3.14, MLIRType{MLIRFloatType{32}}}) == "3.14 : f32");
        assert(print_attr(IndexAttr{0}) == "0 : index");
    }
    // Values
    {
        MLIRValue v{"%x", MLIRType{MLIRIntType{32}}};
        assert(v.name == "%x");
        assert(print_type(v.type) == "i32");
        assert(!v.empty());
    }
    // ConstantOp

    {
        MLIRRegion r;
        r.entry().push<ConstantOp>(MLIRValue{"%c42", MLIRType{MLIRIntType{32}}},
                                   IntAttr{42, MLIRType{MLIRIntType{32}}});

        std::ostringstream os;
        Printer p{os};
        p.print_region(r);
        assert(os.str() == "%c42 = arith.constant 42 : i32\n");
    }

    // BinaryIntOp — same-type result, mnemonic per Kind
    {
        MLIRType i32{MLIRIntType{32}};
        MLIRRegion r;
        auto& bb = r.entry();
        auto& a  = bb.push<ConstantOp>(MLIRValue{"%a", i32}, IntAttr{1, i32});
        auto& b  = bb.push<ConstantOp>(MLIRValue{"%b", i32}, IntAttr{2, i32});
        bb.push<BinaryIntOp>(MLIRValue{"%add", i32}, BinaryIntOp::Kind::Add, a, b);
        bb.push<BinaryIntOp>(MLIRValue{"%mul", i32}, BinaryIntOp::Kind::Mul, a, b);
        bb.push<BinaryIntOp>(MLIRValue{"%shr", i32}, BinaryIntOp::Kind::ShrS, a, b);

        std::ostringstream os;
        Printer p{os};
        p.print_region(r);
        assert(os.str()
               == "%a = arith.constant 1 : i32\n"
                  "%b = arith.constant 2 : i32\n"
                  "%add = arith.addi %a, %b : i32\n"
                  "%mul = arith.muli %a, %b : i32\n"
                  "%shr = arith.shrsi %a, %b : i32\n");
    }

    // CmpiOp — result is i1, trailing type is OPERAND type
    {
        MLIRType i32{MLIRIntType{32}};
        MLIRType i1{MLIRIntType{1}};
        MLIRRegion r;
        auto& bb = r.entry();
        auto& a  = bb.push<ConstantOp>(MLIRValue{"%a", i32}, IntAttr{10, i32});
        auto& b  = bb.push<ConstantOp>(MLIRValue{"%b", i32}, IntAttr{20, i32});
        bb.push<CmpiOp>(MLIRValue{"%eq", i1}, CmpiOp::Pred::Eq, a, b);
        bb.push<CmpiOp>(MLIRValue{"%slt", i1}, CmpiOp::Pred::Slt, a, b);
        bb.push<CmpiOp>(MLIRValue{"%uge", i1}, CmpiOp::Pred::Uge, a, b);

        std::ostringstream os;
        Printer p{os};
        p.print_region(r);
        assert(os.str()
               == "%a = arith.constant 10 : i32\n"
                  "%b = arith.constant 20 : i32\n"
                  "%eq = arith.cmpi eq, %a, %b : i32\n"
                  "%slt = arith.cmpi slt, %a, %b : i32\n"
                  "%uge = arith.cmpi uge, %a, %b : i32\n");
    }

    // void return
    {
        MLIRRegion r;
        r.entry().push_void<FuncReturnOp>();
        std::ostringstream os;
        Printer p{os};
        p.print_region(r);
        assert(os.str() == "func.return\n");
    }

    // single value return
    {
        MLIRType i32{MLIRIntType{32}};
        MLIRRegion r;
        auto c = r.entry().push<ConstantOp>(MLIRValue{"%c", i32}, IntAttr{0, i32});
        r.entry().push_void<FuncReturnOp>(c);
        std::ostringstream os;
        Printer p{os};
        p.print_region(r);
        assert(os.str()
               == "%c = arith.constant 0 : i32\n"
                  "func.return %c : i32\n");
    }

    // FuncOp: func.func public @add(%a: i32, %b: i32) -> i32 { ... }
    {
        MLIRType i32{MLIRIntType{32}};

        std::vector<MLIRValue> args = {
            MLIRValue{"%a", i32},
            MLIRValue{"%b", i32}
        };

        FuncOp func{"add", std::move(args), {i32}};

        // populate the body
        auto& bb    = func.body().entry();
        auto result = bb.push<BinaryIntOp>(MLIRValue{"%r", i32}, BinaryIntOp::Kind::Add, MLIRValue{"%a", i32},
                                           MLIRValue{"%b", i32});
        bb.push_void<FuncReturnOp>(result);

        // wrap in a region and print
        MLIRRegion module;
        module.entry().push_void<FuncOp>(std::move(func));

        std::ostringstream os;
        Printer p{os};
        p.print_region(module);

        assert(os.str()
               == "func.func public @add(%a: i32, %b: i32) -> i32 {\n"
                  "  %r = arith.addi %a, %b : i32\n"
                  "  func.return %r : i32\n"
                  "}\n");
    }

    // AffineForOp: simple loop with an addi in the body
    {
        MLIRType i32{MLIRIntType{32}};
        MLIRType idx{MLIRIndexType{}};

        // build the function
        FuncOp func{"loop_test", {MLIRValue{"%a", i32}}, {i32}};
        auto& func_bb = func.body().entry();

        // build the loop
        AffineForOp loop{
            MLIRValue{"%i", idx},
            0, 10
        };
        auto& loop_bb = loop.body().entry();
        loop_bb.push_void<AffineYieldOp>();

        // push loop into function, then return
        func_bb.push_void<AffineForOp>(std::move(loop));
        func_bb.push_void<FuncReturnOp>(MLIRValue{"%a", i32});

        MLIRRegion module;
        module.entry().push_void<FuncOp>(std::move(func));

        std::ostringstream os;
        Printer p{os};
        p.print_region(module);

        auto actual = os.str();
        std::cout << "=== actual ===\n" << actual << "=== end ===\n";

        assert(actual
               == "func.func public @loop_test(%a: i32) -> i32 {\n"
                  "  affine.for %i = 0 to 10 step 1 {\n"
                  "    affine.yield\n"
                  "  }\n"
                  "  func.return %a : i32\n"
                  "}\n");
    }

    // SCFForOp: loop with SSA bounds
    {
        MLIRType i32{MLIRIntType{32}};
        MLIRType idx{MLIRIndexType{}};

        FuncOp func{"scf_test", {MLIRValue{"%n", idx}}, {}};
        auto& func_bb = func.body().entry();

        // constants for lb and step
        auto lb   = func_bb.push<ConstantOp>(MLIRValue{"%lb", idx}, IndexAttr{0});
        auto step = func_bb.push<ConstantOp>(MLIRValue{"%step", idx}, IndexAttr{1});

        // build the loop — ub is the function argument %n
        SCFForOp loop{
            MLIRValue{"%i", idx},
            lb, MLIRValue{"%n", idx},
            step
        };
        loop.body().entry().push_void<SCFYieldOp>();

        func_bb.push_void<SCFForOp>(std::move(loop));
        func_bb.push_void<FuncReturnOp>();

        MLIRRegion module;
        module.entry().push_void<FuncOp>(std::move(func));

        std::ostringstream os;
        Printer p{os};
        p.print_region(module);

        auto actual = os.str();
        std::cout << "=== actual ===\n" << actual << "=== end ===\n";

        assert(actual
               == "func.func public @scf_test(%n: index) {\n"
                  "  %lb = arith.constant 0 : index\n"
                  "  %step = arith.constant 1 : index\n"
                  "  scf.for %i = %lb to %n step %step {\n"
                  "    scf.yield\n"
                  "  }\n"
                  "  func.return\n"
                  "}\n");
    }

    // LinalgGenericOp: matmul over f64 tensors
    {
        MLIRType f64{MLIRFloatType{64}};

        auto make_tensor = [](std::vector<std::optional<int64_t>> shape, MLIRType elem) -> MLIRType {
            MLIRTensorType t;
            t.shape = std::move(shape);
            t.elem  = std::make_shared<MLIRTypeNode>(std::move(elem));
            return MLIRType{std::move(t)};
        };

        MLIRType tAxK = make_tensor({4, 8}, f64); // A: 4x8
        MLIRType tBxK = make_tensor({8, 4}, f64); // B: 8x4
        MLIRType tCxK = make_tensor({4, 4}, f64); // C: 4x4

        std::vector<MLIRValue> ins = {
            MLIRValue{"%A", tAxK},
            MLIRValue{"%B", tBxK}
        };
        std::vector<MLIRValue> outs = {
            MLIRValue{"%C", tCxK}
        };

        std::vector<std::string> maps = {"affine_map<(d0, d1, d2) -> (d0, d2)>", "affine_map<(d0, d1, d2) -> (d2, d1)>",
                                         "affine_map<(d0, d1, d2) -> (d0, d1)>"};
        std::vector<std::string> iters = {"parallel", "parallel", "reduction"};

        // body args are scalar element types
        std::vector<MLIRValue> body_args = {
            MLIRValue{"%a", f64},
            MLIRValue{"%b", f64},
            MLIRValue{"%c", f64}
        };

        LinalgGenericOp matmul{ins, outs, maps, iters, body_args};

        // populate scalar body
        auto& bb = matmul.body().entry();
        auto mul = bb.push<BinaryFloatOp>(MLIRValue{"%mul", f64}, BinaryFloatOp::Kind::Mul, MLIRValue{"%a", f64},
                                          MLIRValue{"%b", f64});
        auto add = bb.push<BinaryFloatOp>(MLIRValue{"%add", f64}, BinaryFloatOp::Kind::Add, mul, MLIRValue{"%c", f64});
        bb.push_void<LinalgYieldOp>(std::vector<MLIRValue>{add});

        std::ostringstream os;
        Printer p{os};
        matmul.print(p);

        auto actual = os.str();
        std::cout << "=== actual ===\n" << actual << "=== end ===\n";

        assert(actual
               == "%C.out = linalg.generic {"
                  "indexing_maps = ["
                  "affine_map<(d0, d1, d2) -> (d0, d2)>, "
                  "affine_map<(d0, d1, d2) -> (d2, d1)>, "
                  "affine_map<(d0, d1, d2) -> (d0, d1)>"
                  "], "
                  "iterator_types = [\"parallel\", \"parallel\", \"reduction\"]}\n"
                  "  ins(%A, %B : tensor<4x8xf64>, tensor<8x4xf64>)\n"
                  "  outs(%C : tensor<4x4xf64>) {\n"
                  "    ^bb0(%a: f64, %b: f64, %c: f64):\n"
                  "      %mul = arith.mulf %a, %b : f64\n"
                  "      %add = arith.addf %mul, %c : f64\n"
                  "      linalg.yield %add : f64\n"
                  "  }\n"
                  "-> tensor<4x4xf64>\n");
    }

    MLIRValue empty{};
    assert(empty.empty());
    std::cout << "all type tests passed\n";
}
