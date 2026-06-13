#pragma once
#include <mim/lam.h>
#include <mim/phase.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include <mim/plug/tensor/autogen.h>

template<>
struct mim::Axm::IsANode<mim::plug::tensor::broadcast> {
    using type = mim::App;
};

template<>
struct mim::Axm::IsANode<mim::plug::tensor::map_reduce> {
    using type = mim::App;
}; // was not instanciated by autogen

namespace mim::mlir_be {

enum class LamKind {
    Function,      // -> func.func
    AffineForBody, // -> body of affine.for region
    AffineForExit, // -> block after affine.for
    MapReduceBody, // -> linalg.generic body region
    CondBranch,    // -> cf.cond_br or scf.if
    JoinBlock,     // ->block with block args
    Ignored,       // no MLIR representation
};

class LamClassifier : public Analysis {
public:
    explicit LamClassifier(World& w)
        : Analysis(w, "mlir_lam_classifier") {}

    LamKind kind_of(const Lam* lam) const {
        auto it = results_.find(const_cast<Lam*>(lam));
        return it == results_.end() ? LamKind::JoinBlock : it->second;
    }

    // For MapReduceBody lams — returns the containing %tensor.map_reduce App.
    const App* map_reduce_app_of(const Lam* lam) const {
        auto it = map_reduce_apps_.find(const_cast<Lam*>(lam));
        return it == map_reduce_apps_.end() ? nullptr : it->second;
    }
    void run() override {
        Analysis::run();
        //  Classify all externals, since the traversal may miss them if they are not directly reachable from any App
        for (auto& [sym, def] : world().externals())
            if (auto lam = def->isa_mut<Lam>())
                if (!results_.contains(lam))
                    results_[lam] = lam->type()->ret_pi() ? LamKind::Function : LamKind::JoinBlock;
    }

private:
    const Def* rewrite_imm_App(const App* app) override;
    const Def* rewrite_mut_Lam(Lam* lam) override;

    bool is_ignored(Lam* lam) const;
    bool is_cond_branch(Lam* lam) const;

    LamMap<LamKind> results_;
    LamMap<const App*> map_reduce_apps_;
};

} // namespace mim::mlir_be
