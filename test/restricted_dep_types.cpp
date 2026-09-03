#include <doctest/doctest.h>

#include <mim/phase.h>

#include <mim/ast/parser.h>
#include <mim/phase/optimize.h>

#include <mim/plug/compile/compile.h>
#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

using namespace std::literals;
using namespace mim;
using namespace mim::plug;

// TODO can we port this to lit testing?

TEST_CASE("restricted dependent types") {
    Driver driver;
    World& w = driver.world();
    ast::load_plugins(w, {"mem"s, "core"s, "math"s});

    auto i32_t = w.type_i32();
    auto i64_t = w.type_i64();
    auto f32_t = w.annex<math::F32>();

    auto R = w.axm(w.type())->set("R");
    auto W = w.axm(w.type())->set("W");

    auto RW = w.join({w.uniq(R), w.uniq(W)})->set("RW");
    auto DT = w.join({w.uniq(i32_t), w.uniq(i64_t)})->set("DT");

    auto exp_pi = w.mut_pi(w.type<1>())->set_dom({DT, RW});
    exp_pi->set_codom(w.type());
    auto Exp = w.axm(exp_pi)->set("exp");

    auto exp = [&](const Def* dt, const Def* rw) { return w.app(Exp, {w.inj(DT, dt), w.inj(RW, rw)}); };

    SUBCASE("the mode is a parameter") {
        auto exp_sig = w.mut_sigma(4);
        exp_sig->set(0, w.type());
        exp_sig->set(1, w.type());
        exp_sig->set(2, exp(exp_sig->var(0uz), exp_sig->var(1uz)));
        exp_sig->set(3, w.cn(exp_sig->var(0uz)));

        auto exp_lam = w.mut_con(exp_sig);
        exp_lam->app(false, exp_lam->var(3), w.call<core::bitcast>(exp_lam->var(0uz), exp_lam->var(2uz)));

        auto app = [&](const Def* dt, const Def* rw, const Def* lit_t) {
            return w.app(exp_lam, {dt, rw, w.call<core::bitcast>(exp(dt, rw), w.lit(lit_t, 1000)), w.mut_con(dt)});
        };

        CHECK_NOTHROW(app(i32_t, R, i32_t));
        CHECK_NOTHROW(app(i32_t, W, i32_t));
        CHECK_NOTHROW(app(i64_t, R, i32_t));
        CHECK_NOTHROW(app(i64_t, W, i32_t));

        // Nothing rejects these yet; vel type checking must turn each into a CHECK_THROWS_AS.
        CHECK_NOTHROW(app(f32_t, R, i32_t)); // f32 is not in DT
        CHECK_NOTHROW(app(f32_t, W, i32_t));
        CHECK_NOTHROW(app(i32_t, i32_t, i32_t)); // i32 is not in RW
        CHECK_NOTHROW(app(i64_t, i64_t, i32_t));
    }

    SUBCASE("the mode is fixed to R") {
        auto exp_sig = w.mut_sigma(3);
        exp_sig->set(0, w.type());
        exp_sig->set(1, exp(exp_sig->var(0uz), R));
        exp_sig->set(2, w.cn(exp_sig->var(0uz)));

        auto exp_lam = w.mut_con(exp_sig);
        exp_lam->app(false, exp_lam->var(2uz), w.call<core::bitcast>(exp_lam->var(0uz), exp_lam->var(1uz)));

        auto app = [&](const Def* dt, const Def* rw, const Def* lit_t) {
            return w.app(exp_lam, {dt, w.call<core::bitcast>(exp(dt, rw), w.lit(lit_t, 1000)), w.mut_con(dt)});
        };

        CHECK_NOTHROW(app(i32_t, R, i32_t));
        CHECK_NOTHROW(app(i64_t, R, i64_t));

        // Nothing rejects this yet; vel type checking must turn it into a CHECK_THROWS_AS.
        CHECK_NOTHROW(app(f32_t, R, i32_t)); // f32 is not in DT

        CHECK_THROWS(app(i32_t, W, i32_t)); // the lam only accepts R
        CHECK_THROWS(app(i64_t, W, i32_t));
        CHECK_THROWS(app(f32_t, W, f32_t));
    }
}

TEST_CASE("restricted dependent types: ll") {
    Driver driver;
    World& w = driver.world();
    w.set("restricted_dep_types");
    ast::load_plugins(w, {"mem"s, "core"s, "math"s, "ll"s});

    auto mem_t  = w.call<mem::M>(0);
    auto i32_t  = w.type_i32();
    auto argv_t = w.call<mem::Ptr0>(w.call<mem::Ptr0>(i32_t));

    // Cn [mem, i32, ptr(ptr(i32, 0), 0) Cn [mem, i32]]
    auto main = w.mut_con({mem_t, i32_t, argv_t, w.cn({mem_t, i32_t})})->set("main");
    main->externalize();

    auto R = w.axm(w.type())->set("R");
    auto W = w.axm(w.type())->set("W");

    auto RW = w.join({w.uniq(R), w.uniq(W)})->set("RW");

    auto DT     = w.join({w.uniq(i32_t), w.uniq(w.annex<math::F32>())})->set("DT");
    auto exp_pi = w.mut_pi(w.type<1>())->set_dom({DT, RW});
    exp_pi->set_codom(w.type());

    auto Exp = w.axm(exp_pi)->set("exp");

    auto app_exp = w.app(Exp, {w.inj(DT, i32_t), w.inj(RW, R)});

    auto exp_sig = w.mut_sigma(5);
    exp_sig->set(0, mem_t);
    exp_sig->set(1, w.type());
    exp_sig->set(2, w.type());
    exp_sig->set(3, w.app(Exp, {w.inj(DT, exp_sig->var(1uz)), w.inj(RW, exp_sig->var(2uz))}));
    exp_sig->set(4, w.cn({mem_t, i32_t}));

    auto exp_lam = w.mut_con(exp_sig);
    auto bc      = w.call<core::bitcast>(i32_t, exp_lam->var(3uz));
    exp_lam->app(false, exp_lam->var(4), {exp_lam->var(0uz), bc});

    main->app(false, exp_lam, {main->var(0uz), i32_t, R, w.call<core::bitcast>(app_exp, main->var(1)), main->var(3)});

    // the `ll` plugin's emit phase writes `restricted_dep_types.ll` as part of `optimize`
    optimize(w);
}
