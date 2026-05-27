#include <cassert>

#include <iostream>

#include <mim/driver.h>

#include <mim/ast/parser.h>

#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/core/be/mlir/region_tree.h"
#include "mim/plug/core/be/mlir/type_converter.h"

using namespace mim;
using namespace mim::mlir_be;

static std::string cvt(TypeConverter& tc, const Def* type) { return print_type(tc.convert(type)); }

int main() {
    // Nat
    {
        Driver driver;
        World& w = driver.world();
        TypeConverter tc{w};
        assert(cvt(tc, w.type_nat()) == "index");
        std::cout << "Nat: ok\n";
    }

    // Idx
    {
        Driver driver;
        World& w = driver.world();
        TypeConverter tc{w};
        assert(cvt(tc, w.type_idx(w.lit_nat(2))) == "i1");
        assert(cvt(tc, w.type_idx(w.lit_nat(0x100))) == "i8");
        assert(cvt(tc, w.type_idx(w.lit_nat(0x1'0000))) == "i16");
        assert(cvt(tc, w.type_idx(w.lit_nat(0x1'0000'0000))) == "i32");
        assert(cvt(tc, w.type_idx(w.lit_nat(0))) == "i64");
        std::cout << "Idx: ok\n";
    }

    // Float
    {
        Driver driver;
        World& w = driver.world();
        ast::load_plugins(w, "math");
        TypeConverter tc{w};
        assert(cvt(tc, plug::math::type_f(w, 10, 5)) == "f16");
        assert(cvt(tc, plug::math::type_f(w, 23, 8)) == "f32");
        assert(cvt(tc, plug::math::type_f(w, 52, 11)) == "f64");
        std::cout << "Float: ok\n";
    }

    // Arr static
    {
        Driver driver;
        World& w = driver.world();
        TypeConverter tc{w};
        auto arr = w.arr(w.lit_nat(4), w.type_nat());
        assert(cvt(tc, arr) == "tensor<4xindex>");
        std::cout << "Arr static: ok\n";
    }

    // Arr nested
    {
        Driver driver;
        World& w = driver.world();
        ast::load_plugins(w, "math");
        TypeConverter tc{w};
        auto f32   = plug::math::type_f(w, 23, 8);
        auto inner = w.arr(w.lit_nat(8), f32);
        auto outer = w.arr(w.lit_nat(4), inner);
        assert(cvt(tc, outer) == "tensor<4x8xf32>");
        std::cout << "Arr nested: ok\n";
    }

    // Ptr
    {
        Driver driver;
        World& w = driver.world();
        ast::load_plugins(w, "mem");
        TypeConverter tc{w};
        auto ptr = w.app(w.annex<plug::mem::Ptr>(), w.tuple({w.type_nat(), w.lit_nat(0)}));
        assert(cvt(tc, ptr) == "memref<index>");
        std::cout << "Ptr: ok\n";
    }

    std::cout << "all type converter tests passed\n";
    return 0;
}
