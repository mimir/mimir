#include <sstream>

#include <doctest/doctest.h>

#include <mim/check.h>
#include <mim/world.h>

#include <mim/ast/ast.h>
#include <mim/ast/parser.h>

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>

using namespace std::literals;
using namespace mim;

namespace {

/// Dumps a Def, parses the dump back into the very same World, and checks that both are α-equivalent.
class RoundTrip {
public:
    RoundTrip(std::vector<std::string> plugins = {})
        : ast_(driver_.world())
        , parser_(ast_) {
        for (const auto& plugin : plugins)
            prelude_ += std::format("plugin {};\n", plugin);
        if (!prelude_.empty()) parse(prelude_);
    }

    World& world() { return driver_.world(); }

    bool operator()(const Def* def) {
        note_    = std::format("{}", def);
        auto got = parse(std::format("{}let _rt = {};", prelude_, note_));
        if (!got || !Checker::alpha<Checker::Test>(def, got)) {
            note_ += std::format("   -->   {}", got);
            return false;
        }
        return true;
    }

    std::string_view note() const { return note_; }

private:
    const Def* parse(std::string code) {
        auto is = std::istringstream(std::move(code));
        try {
            auto file = parser_.import(is, std::format("rt{}.mim", n_++));
            if (!file) return nullptr;
            file->compile(ast_);
            for (auto decl : file->decls())
                if (auto let = decl->isa<ast::LetDecl>()) return let->def();
        } catch (const Error::Bail&) { note_ += " [error]"; }
        return nullptr;
    }

    Driver driver_;
    ast::AST ast_;
    ast::Parser parser_;
    std::string prelude_;
    std::string note_;
    size_t n_ = 0;
};

} // namespace

#define CHECK_RT(def) CHECK_MESSAGE(rt(def), rt.note())

TEST_CASE("dump: types") {
    auto rt = RoundTrip();
    auto& w = rt.world();
    CHECK_RT(w.type_nat());
    CHECK_RT(w.type_idx());
    CHECK_RT(w.univ());
    CHECK_RT(w.type<0>());
    CHECK_RT(w.type<1>());
    CHECK_RT(w.type(w.lit_univ(2)));
    CHECK_RT(w.type_bool());
    CHECK_RT(w.type_i8());
    CHECK_RT(w.type_i16());
    CHECK_RT(w.type_i32());
    CHECK_RT(w.type_i64());
    CHECK_RT(w.type_idx(23));
}

TEST_CASE("dump: lits") {
    auto rt = RoundTrip();
    auto& w = rt.world();
    CHECK_RT(w.lit_nat_0());
    CHECK_RT(w.lit_nat(23));
    CHECK_RT(w.lit_nat(0x100));
    CHECK_RT(w.lit_nat(0x10000));
    CHECK_RT(w.lit_nat(0x100000000));
    CHECK_RT(w.lit_ff());
    CHECK_RT(w.lit_tt());
    CHECK_RT(w.lit_i8(5));
    CHECK_RT(w.lit_i16(7));
    CHECK_RT(w.lit_i32(9));
    CHECK_RT(w.lit_i64(11));
    CHECK_RT(w.lit_idx(10, 3));
    CHECK_RT(w.lit_idx(23, 7));
    CHECK_RT(w.bot(w.type_nat()));
    CHECK_RT(w.top(w.type_i32()));
    CHECK_RT(w.bot(w.type<0>()));
}

TEST_CASE("dump: tuples, sigmas, arrs, packs") {
    auto rt  = RoundTrip();
    auto& w  = rt.world();
    auto nat = w.type_nat();
    auto t23 = w.lit_nat(23);
    CHECK_RT(w.tuple());
    CHECK_RT(w.sigma());
    CHECK_RT(w.tuple({t23, w.lit_nat(42)}));
    CHECK_RT(w.tuple({t23, w.tuple({t23, t23})}));
    CHECK_RT(w.sigma({nat, nat}));
    CHECK_RT(w.sigma({nat, w.sigma({nat, w.type_i32()})}));
    CHECK_RT(w.arr(5, nat));
    CHECK_RT(w.pack(5, t23));
    CHECK_RT(w.arr(5, w.arr(7, nat)));
    CHECK_RT(w.pack(5, w.pack(7, t23)));

    SUBCASE("dependent") {
        auto sig = w.mut_sigma(w.type<0>(), 2);
        sig->set(0, nat);
        sig->set(1, w.type_idx(sig->var(2, 0)));
        CHECK_RT(sig);

        auto types = w.tuple({nat, w.type_i32(), w.type_i64()});
        auto arr   = w.mut_arr(w.type<0>())->set_shape(w.lit_nat(3));
        arr->set_body(w.extract(types, arr->var()));
        CHECK_RT(arr);

        auto nats = w.tuple({t23, w.lit_nat(42), w.lit_nat(7)});
        auto pack = w.mut_pack(w.arr(3, nat))->set_shape(w.lit_nat(3));
        pack->set_body(w.extract(nats, pack->var()));
        CHECK_RT(pack);
    }
}

TEST_CASE("dump: fused shapes") {
    auto rt  = RoundTrip();
    auto& w  = rt.world();
    auto nat = w.type_nat();
    auto t23 = w.lit_nat(23);
    CHECK_RT(w.arr(Defs{w.lit_nat(2), w.lit_nat(3)}, nat));
    CHECK_RT(w.arr(Defs{w.lit_nat(2), w.lit_nat(3), w.lit_nat(4)}, nat));
    CHECK_RT(w.pack(Defs{w.lit_nat(2), w.lit_nat(3)}, t23));
    CHECK_RT(w.arr(Defs{w.lit_nat(2), w.lit_nat(3)}, w.pi(nat, nat)));
    CHECK_RT(w.pi(w.arr(Defs{w.lit_nat(2), w.lit_nat(3)}, nat), nat));
    CHECK(std::format("{}", w.arr(Defs{w.lit_nat(2), w.lit_nat(3)}, nat)) == "«2, 3; Nat»");
    CHECK(std::format("{}", w.pack(Defs{w.lit_nat(2), w.lit_nat(3)}, t23)) == "‹2, 3; 23›");

    SUBCASE("dependent") {
        auto types = w.tuple({nat, w.type_i32(), w.type_i64()});
        auto arr   = w.mut_arr(w.type<0>())->set_shape(w.tuple({w.lit_nat(3), w.lit_nat(2)}));
        arr->set_body(w.extract(types, w.extract(arr->var(), w.lit_idx(2, 0))));
        CHECK_RT(arr);
    }
}

TEST_CASE("dump: extract & insert") {
    auto rt  = RoundTrip();
    auto& w  = rt.world();
    auto t23 = w.lit_nat(23);
    auto tup = w.tuple({t23, w.lit_nat(42)});
    auto i0  = w.lit_idx(2, 0);
    auto i1  = w.lit_idx(2, 1);
    CHECK_RT(w.extract(tup, i0));
    CHECK_RT(w.insert(tup, i0, w.lit_nat(7)));
    auto nested = w.tuple({tup, tup});
    CHECK_RT(w.extract(w.extract(nested, i0), i1));
    CHECK_RT(w.insert(w.extract(nested, i0), i1, t23));
    CHECK_RT(w.insert(nested, i0, w.insert(tup, i1, t23)));
}

TEST_CASE("dump: pis") {
    auto rt  = RoundTrip();
    auto& w  = rt.world();
    auto nat = w.type_nat();
    auto i32 = w.type_i32();
    CHECK_RT(w.pi(nat, i32));
    CHECK_RT(w.pi(nat, w.pi(i32, nat)));
    CHECK_RT(w.pi(w.pi(nat, i32), nat));
    CHECK_RT(w.cn(nat));
    CHECK_RT(w.cn({nat, i32}));
    CHECK_RT(w.pi(w.sigma({nat, i32}), nat));
    CHECK_RT(w.pi(nat, w.cn(i32)));
    CHECK_RT(w.cn({nat, w.cn(i32)})); // a returning continuation - `Fn`
    CHECK_RT(w.pi(w.arr(3, nat), w.join({nat, i32})));

    SUBCASE("dependent") {
        auto pi = w.mut_pi(w.type<1>())->set_dom(w.type<0>());
        pi->set_codom(w.pi(pi->var(), pi->var()));
        CHECK_RT(pi);

        auto impl = w.mut_pi(w.type<1>(), true)->set_dom(w.type<0>());
        impl->set_codom(w.pi(impl->var(), impl->var()));
        CHECK_RT(impl);
    }

    SUBCASE("implicit without binder") { CHECK_RT(w.pi(nat, i32, true)); }
}

TEST_CASE("dump: bounds") {
    auto rt  = RoundTrip();
    auto& w  = rt.world();
    auto nat = w.type_nat();
    auto i32 = w.type_i32();
    auto j   = w.join({nat, i32});
    CHECK_RT(j);
    CHECK_RT(w.join({nat, w.join({i32, w.type_i8()})}));
    CHECK_RT(w.join({nat, i32, w.type_i8()}));
    CHECK_RT(w.single(nat));
    CHECK_RT(w.single(j));
    CHECK_RT(w.wrap(nat));
    CHECK_RT(w.wrap(w.lit_nat(23)));
    CHECK_RT(w.inj(j, w.lit_nat(23)));
    CHECK_RT(w.join({w.single(nat), w.single(i32)}));

    // `∩` has no surface syntax, so a Meet cannot round-trip; see `∪` in docs/langref.md.
    CHECK(std::format("{}", w.meet({nat, i32})) == "∩(Nat, I32)");
}

TEST_CASE("dump: apps") {
    auto rt  = RoundTrip({"core"s});
    auto& w  = rt.world();
    auto i32 = w.type_i32();
    auto a   = w.lit_i32(2);
    auto b   = w.lit_i32(3);
    auto add = w.annex(plug::core::wrap::add);
    CHECK_RT(add);
    CHECK_RT(w.app(add, w.lit_nat_0()));                                // `[m: Nat]` is explicit
    CHECK_RT(w.app(w.app(add, w.lit_nat_0()), w.lit_nat(0x100000000))); // `{s: Nat}` is implicit
    CHECK_RT(w.call(plug::core::wrap::add, plug::core::Mode::none, Defs{a, w.top(i32)}));
    CHECK_RT(w.call(plug::core::wrap::add, plug::core::Mode::none, Defs{a, b})); // folds to a Lit
    CHECK_RT(w.call(plug::core::nat::add, Defs{w.lit_nat(2), w.lit_nat(3)}));
    CHECK_RT(w.type_idx(w.call(plug::core::nat::add, Defs{w.lit_nat(2), w.lit_nat(3)})));
    CHECK_RT(w.reform(w.type_nat()));
}

TEST_CASE("dump: math lits") {
    auto rt  = RoundTrip({"math"s});
    auto& w  = rt.world();
    auto f32 = w.annex<plug::math::F32>();
    auto f64 = w.annex<plug::math::F64>();
    CHECK_RT(f32);
    CHECK_RT(f64);
    CHECK_RT(w.lit(f32, 0));
    CHECK_RT(w.lit(f64, 0x4000000000000000));
    CHECK_RT(w.arr(3, f64));
}

TEST_CASE("dump: precedence") {
    auto rt  = RoundTrip({"core"s});
    auto& w  = rt.world();
    auto nat = w.type_nat();
    auto i32 = w.type_i32();
    auto i8  = w.type_i8();
    auto t   = w.tuple({w.lit_nat(23), w.lit_nat(42)});
    auto i0  = w.lit_idx(2, 0);
    auto i1  = w.lit_idx(2, 1);

    SUBCASE("golden") {
        auto add = w.call(plug::core::wrap::add, plug::core::Mode::none, Defs{w.lit_i8(255), w.top(w.type_i8())});
        // clang-format off
        CHECK(std::format("{}", w.pi(w.pi(nat, i32), nat))    == "(Nat → I32) → Nat");
        CHECK(std::format("{}", w.pi(nat, w.pi(i32, nat)))    == "Nat → I32 → Nat");
        CHECK(std::format("{}", w.pi(w.join({nat, i32}), i8)) == "(Nat ∪ I32) → I8");
        CHECK(std::format("{}", w.join({w.pi(nat, i32), i8})) == "I8 ∪ Nat → I32");
        CHECK(std::format("{}", w.inj(w.join({nat, i32}), w.lit_nat(23))) == "23 inj Nat ∪ I32");
        CHECK(std::format("{}", w.arr(3, w.pi(nat, i32)))     == "«3; Nat → I32»");
        CHECK(std::format("{}", add)                          == "core.wrap.add 0 @ i8 (255I8, ⊤:I8)");
        CHECK(std::format("{}", w.single(w.pi(nat, i32)))     == "«Nat → I32»");
        CHECK(std::format("{}", w.wrap(w.lit_nat(23)))        == "‹23›");
        // clang-format on
    }

    SUBCASE("round-trip") {
        CHECK_RT(w.pi(w.join({nat, i32}), i8));
        CHECK_RT(w.join({w.pi(nat, i32), i8}));
        CHECK_RT(w.arr(3, w.pi(nat, i32)));
        CHECK_RT(w.pi(w.arr(3, nat), w.arr(3, i32)));
        CHECK_RT(w.single(w.pi(nat, i32)));
        CHECK_RT(w.wrap(w.pi(nat, i32)));
        CHECK_RT(w.join({w.single(nat), w.single(i32)}));
        CHECK_RT(w.inj(w.join({nat, i32}), w.lit_nat(23)));
        CHECK_RT(w.extract(t, i0));
        CHECK_RT(w.insert(t, i0, w.lit_nat(7)));
        CHECK_RT(w.insert(w.insert(t, i0, w.lit_nat(7)), i1, w.lit_nat(8)));
        CHECK_RT(w.call(plug::core::wrap::add, plug::core::Mode::none, Defs{w.lit_i8(255), w.top(w.type_i8())}));
        CHECK_RT(w.type_idx(w.call(plug::core::nat::add, Defs{w.lit_nat(2), w.lit_nat(3)})));
        CHECK_RT(w.arr(w.call(plug::core::nat::add, Defs{w.lit_nat(2), w.lit_nat(3)}), nat));
    }
}
