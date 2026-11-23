#include <format>
#include <sstream>

#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/rule.h>

#include <mim/ast/parser.h>

using namespace mim;

namespace {

/// Checks the C++ normalizer generated from the `norm` rules of @p plugin against Rule::fire on the body of each
/// `extern lam` in @p terms; a `yes_` lam must fire, any other must not.
void differential(std::string_view plugin, std::string_view terms) {
    Driver driver;
    auto& w = driver.world();
    w.set("normtest");
    auto ast    = ast::AST(w);
    auto parser = ast::Parser(ast);
    // Without its library, as during bootstrap, the plugin normalizes neither its rules nor the terms.
    auto source = std::istringstream(std::format("import {};\n{}", plugin, terms));
    auto file   = parser.import(source, "normtest.mim");
    REQUIRE(file);
    file->bind(ast);
    ast.error().ack();
    file->compile(ast);

    // The generated normalizers refer to the plugin's annexes, which the World only attaches once it is loaded.
    driver.load(plugin);
    for (auto [flags, entry] : ast.flags2annex())
        if (!fe::lookup(w.annexes().flags2entry(), flags)) w.annexes().attach(flags, entry.sym, entry.def);

    const auto& rules = ast.plugin2norms(w.sym(plugin));
    REQUIRE(!rules.empty());

    size_t num_terms = 0;
    for (auto mut : w.externals().muts()) {
        auto lam = mut->isa<Lam>();
        if (!lam) continue;
        INFO("term `", lam->sym(), "`");
        auto app = lam->body()->as<App>();

        auto fired = static_cast<const Def*>(nullptr);
        for (auto decl : rules)
            if ((fired = decl->def()->as<Rule>()->fire(app))) break;

        auto [axm, _, __] = Axm::get(app);
        auto normalizer   = driver.normalizer(axm->flags());
        REQUIRE(normalizer);
        auto generated = normalizer(app->type(), app->callee(), app->arg());

        CHECK(generated == fired);
        CHECK((generated != nullptr) == lam->sym().view().starts_with("yes_"));
        ++num_terms;
    }
    CHECK(num_terms > 0);
}

} // namespace

TEST_CASE("norm: generated normalizers agree with Rule::fire") {
    SUBCASE("demo") {
        differential("demo", R"(
            extern lam yes_big (): Idx 100 = demo.const_idx 100;
            extern lam no_small (): Idx 10 = demo.const_idx 10;
            extern lam no_var (n: Nat): Idx n = demo.const_idx n;
        )");
    }

    SUBCASE("clos") {
        differential("clos", R"(
            extern lam yes_bottom (x: Nat): Nat = clos.anno.bottom x;
            extern lam yes_poly (T: *, x: T): T = clos.anno.bottom x;
            extern lam no_esc (x: Nat): Nat = clos.anno.esc x;
        )");
    }

    SUBCASE("option") {
        differential("option", R"(
            extern lam yes_some (x: Nat): Nat = option.unwrap_unsafe (option.Opt#Some x);
            extern lam yes_lit (): I32 = option.unwrap_unsafe (option.Opt#Some 23I32);
            extern lam no_none (): Nat = option.unwrap_unsafe (option.Opt#None);
            extern lam no_var (o: option.Opt@Nat): Nat = option.unwrap_unsafe o;
        )");
    }

    SUBCASE("buffer") {
        differential("buffer", R"(
            extern lam yes_lit (b: buffer.Buf (2, (3, 5), I32), i: Idx 2): Nat = buffer.shape (2, (3, 5), I32) (b, i);
            extern lam yes_var (rsT: [r: Nat, s: «r; Nat», T: *], b: buffer.Buf rsT, i: Idx rsT#0₃): Nat =
                buffer.shape rsT (b, i);
        )");
    }
}
