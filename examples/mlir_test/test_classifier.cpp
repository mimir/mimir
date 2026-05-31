#include <cassert>

#include <filesystem>
#include <fstream>
#include <iostream>

#include <mim/driver.h>
#include <mim/lam.h>

#include <mim/ast/parser.h>

#include "mim/plug/core/be/mlir/lam_classifier.h"

using namespace mim;
using namespace mim::mlir_be;
namespace fs = std::filesystem;

static const char* kind_str(LamKind k) {
    switch (k) {
        case LamKind::Function: return "Function";
        case LamKind::AffineForBody: return "AffineForBody";
        case LamKind::AffineForExit: return "AffineForExit";
        case LamKind::MapReduceBody: return "MapReduceBody";
        case LamKind::CondBranch: return "CondBranch";
        case LamKind::JoinBlock: return "JoinBlock";
        case LamKind::Ignored: return "Ignored";
    }
    return "?";
}

int main() {
    // -------------- map_reduce detection -------------
    {
        Driver driver;
        World& w     = driver.world();
        auto ast_obj = ast::AST(w);
        auto parser  = ast::Parser(ast_obj);

        fs::path p(MIM_SOURCE_DIR "/lit/tensor/map_reduce.mim");
        std::ifstream f{p};
        assert(f.is_open());

        if (auto mod = parser.import(f, {}, &p)) {
            mod->compile(ast_obj);

            LamClassifier clf{w};
            clf.run();

            bool found_map_reduce_body = false;
            w.for_each<Lam>(true, [&](Lam* lam) {
                auto kind = clf.kind_of(lam);
                std::cout << "  " << lam->sym().str() << " → " << kind_str(kind) << "\n";
                if (kind == LamKind::MapReduceBody) {
                    found_map_reduce_body = true;
                    assert(clf.map_reduce_app_of(lam) && "map_reduce app should be stored");
                }
            });

            assert(found_map_reduce_body && "no MapReduceBody found");
            std::cout << "map_reduce detection: ok\n";
        }
    }

    // ----------------- affine.For detection --------------------
    {
        Driver driver;
        World& w     = driver.world();
        auto ast_obj = ast::AST(w);
        auto parser  = ast::Parser(ast_obj);

        fs::path p(MIM_SOURCE_DIR "/lit/affine/for_1acc.mim");
        std::ifstream f{p};
        assert(f.is_open());

        if (auto mod = parser.import(f, {}, &p)) {
            mod->compile(ast_obj);

            LamClassifier clf{w};
            clf.run();

            bool found_affine_body = false;
            // for_exit is a local lam, but correctly marked in results_
            w.for_each<Lam>(true, [&](Lam* lam) {
                auto kind = clf.kind_of(lam);
                std::cout << "  " << lam->sym().str() << " → " << kind_str(kind) << "\n";
                if (kind == LamKind::AffineForBody) found_affine_body = true;
            });

            assert(found_affine_body && "no AffineForBody found");
            std::cout << "affine.For detection: ok\n";
        }
    }

    std::cout << "all classifier tests passed\n";
    return 0;
}
