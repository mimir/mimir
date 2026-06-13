#pragma once
#include <ostream>
#include <set>
#include <string>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/core/be/mlir/lam_classifier.h"
#include "mim/plug/core/be/mlir/ops/arith.h"
#include "mim/plug/core/be/mlir/ops/func.h"
#include "mim/plug/core/be/mlir/ops/linalg.h"
#include "mim/plug/core/be/mlir/ops/math.h"
#include "mim/plug/core/be/mlir/ops/memref.h"
#include "mim/plug/core/be/mlir/ops/scf.h"
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
    // top-level
    void emit_func(Lam* lam, MLIRBlock& into);

    // -----region builders-----
    void emit_affine_for(const App* app, MLIRBlock& into);

    void emit_for_body(Lam* body_lam, MLIRBlock& body_bb);

    void emit_body(Lam* lam, MLIRBlock& into);

    // ------ leaf def emitter ---------
    MLIRValue emit_def(const Def* def, MLIRBlock& into);
    MLIRValue get_or_emit(const Def* def, MLIRBlock& into);

    void emit_linalg_generic(const App* mr_app, MLIRBlock& into);
    void emit_linalg_body(Lam* body_lam, MLIRBlock& body_bb);

    //  -------arg seeding -----------
    void seed_dom_op(const Def* op, std::vector<MLIRValue>& args);
    void seed_var_tree(const Def* d);

    // ----- helpers  ---------
    std::string fresh_name(const Def* def);
    std::string fresh_name(std::string prefix);
    bool is_return_callee(const Def* c, const Def* ret_var);

    World& world_;
    std::ostream& os_;
    TypeConverter types_;
    LamClassifier clf_;

    DefMap<MLIRValue> values_;

    const Def* curr_ret_var_ = nullptr;
    int name_counter_        = 0;
};

} // namespace mim::mlir_be
