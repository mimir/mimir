#pragma once
#include <mim/world.h>

#include "mim/plug/core/be/mlir/ops/arith.h"
#include "mim/plug/core/be/mlir/region_tree.h"

namespace mim::mlir_be {

class TypeConverter {
public:
    explicit TypeConverter(World& w)
        : world_(w) {}

    MLIRType convert(const Def* type);
    std::string ret_type_str(const Pi* pi);
    bool is_void_ret(const Pi* pi) const;

    MLIRValue to_index(MLIRValue v, MLIRBlock& into, std::string fresh_name);

private:
    World& world_;
    DefMap<MLIRType> cache_;
};

} // namespace mim::mlir_be
