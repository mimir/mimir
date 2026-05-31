#pragma once
#include <ostream>
#include <string>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/core/be/mlir/lam_classifier.h"
#include "mim/plug/core/be/mlir/ops/arith.h"
#include "mim/plug/core/be/mlir/ops/func.h"
#include "mim/plug/core/be/mlir/printer.h"
#include "mim/plug/core/be/mlir/region_tree.h"
#include "mim/plug/core/be/mlir/type_converter.h"

namespace mim::mlir_be {

class MLIREmitter {
public:
    MLIREmitter(World& world, std::ostream& os)
        : world_(world)
        , os_(os)
        , types_(world)
        , clf_(world) {}

    void run();

private:
    void emit_body(Lam* lam, MLIRBlock& into);

    void emit_func(Lam* lam, MLIRBlock& into);

    MLIRValue emit_def(const Def* def, MLIRBlock& into);

    MLIRValue get_or_emit(const Def* def, MLIRBlock& into);

    std::string fresh_name(const Def* def);
    void preseed_vars(const Def* def);
    void seed_dom_op(const Def* op, std::vector<MLIRValue>& args);

    World& world_;
    std::ostream& os_;
    TypeConverter types_;
    LamClassifier clf_;

    DefMap<MLIRValue> values_;

    int name_counter_ = 0;
};

} // namespace mim::mlir_be
