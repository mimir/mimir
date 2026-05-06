#include <mim/ast/parser.h>
#include <mim/util/sys.h>

#include <mim/plug/mem/mem.h>
#include <mim/phase/eta_exp_phase.h>
#include <mim/phase/beta_red_phase.h>
#include <mim/phase/eta_red_phase.h>

#include "sccp.h"

auto constexpr prog = R"(
plugin core;

cfun dont_know[]: Bool;

fun extern f(x: Nat): Nat =
    head (23, x, 3) where
        con head(c x t: Nat) =      // after SCCP: c → 23, x → x, t → top
            ret cond = dont_know $ ();
            (exit, body)#cond () where
                con body() =
                    let cond       = %core.ncmp.e (c, 23);
                    (false, true)#cond ()
                    where
                        con true () =
                            next (c, x, 4);

                        con false() =
                            let c = %core.nat.add (c, 1);
                            let x = %core.nat.add (x, 1);
                            next (c, x, t);
                        con next (c x t: Nat) as cxt = head cxt;
                    end;

                con exit() =
                    let r = %core.nat.add (c, x);
                    let s = %core.nat.add (r, t);
                    return s;
            end
    end;
)";

int main(int, char**) {
    try {
        auto driver = mim::Driver();
        auto& world = driver.world();
        auto ast    = mim::ast::AST(world);
        auto parser = mim::ast::Parser(ast);
        auto is     = std::istringstream(prog);

        auto path = std::filesystem::path("prog.mim");
        if (auto mod = parser.import(is, mim::Loc(&path, mim::Pos(1, 1), mim::Pos(1, 1)))) {
            mod->compile(ast);
            mim::Phase::run<mim::BetaRedPhase>(world);
            mim::Phase::run<mim::EtaRedPhase>(world);
            mim::Phase::run<mim::EtaExpPhase>(world);
            mim::Phase::run<mim::SCCP>(world);
            world.dump();
        }
    } catch (const std::exception& e) {
        mim::errln("{}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        mim::errln("error: unknown exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

