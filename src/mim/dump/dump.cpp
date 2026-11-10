#include <fstream>
#include <ostream>

#include <fe/assert.h>

#include "mim/driver.h"

#include "mim/ast/lexer.h"

#include "dumper.h"

using namespace std::literals;

namespace mim {

namespace {

/// The name of the file a World is dumped to - and hence the module its own annexes live in.
std::string file_stem(World& world) { return (world.name() ? world.name() : world.sym("_default")).str(); }

/// Spells out the `import`s again; yields their files and adds the modules they name to @p reserved.
dump::Srcs dump_imports(std::ostream& os, const Driver& driver, fe::Vector<std::string>& reserved) {
    auto srcs = dump::Srcs();
    for (const auto& import : driver.imports()) {
        srcs.emplace(import.src);
        auto kw     = import.tag == ast::Tok::Tag::K_plugin ? "plugin" : "import";
        auto module = import.src->path().stem().string();
        // A stem that is no identifier names no module, so the file has to be spliced instead.
        auto is_mod = ast::Lexer::is_id(module);
        if (is_mod) reserved.emplace_back(std::move(module));
        // The spelling was relative to the importing file; only the resolved path re-parses from here.
        // Generic format: a native Windows `\` would lex as an escape sequence inside the string literal.
        if (import.path)
            std::print(os, "{} \"{}\"{};\n", kw, ast::Lexer::escape(import.src->path().generic_string()),
                       is_mod ? "" : " as *");
        else
            std::print(os, "{} {};\n", kw, import.sym);
    }
    return srcs;
}

/// The generic `anx nom`s from elsewhere - the Nom%s the annex Lam%s of an `import` return.
/// @p self is the `<world name>.` prefix of the annexes the dump declares itself.
dump::Generics generics(World& world, std::string_view self) {
    auto res = dump::Generics();
    auto lib = world.driver().library();
    for (auto w : {&world, lib == &world ? nullptr : lib})
        if (w)
            for (auto [sym, def] : w->annexes().entries())
                if (auto lam = def->isa_mut<Lam>(); lam && !sym.view().starts_with(self)) {
                    auto chain = dump::curry_chain(lam);
                    if (auto body = chain.back()->body())
                        if (auto nom = body->isa<Nom>()) {
                            auto implicit = fe::Vector<bool>();
                            for (auto l : chain)
                                implicit.emplace_back(l->type()->is_implicit());
                            res.emplace(nom->key(), dump::Generic{sym, nom, std::move(implicit)});
                        }
                }
    return res;
}

/// What the externals reach.
struct Reached {
    DefSet live;
    fe::Vector<dump::DumpAnnex> annexes; ///< The annexes of the dumped file itself, in declaration order.
};

/// An `import` declares its own annexes; those of the dumped file itself only exist if the dump declares them.
/// Only a loaded plugin registers its annexes, so find them by what the externals reach.
Reached reach(World& world, std::string_view self) {
    auto res  = Reached();
    auto todo = DefVec(world.externals().muts().begin(), world.externals().muts().end());
    res.live.insert(todo.begin(), todo.end());
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if ((def->isa<Axm>() || dump::is_anx_nom(def)) && def->sym().view().starts_with(self)) {
            auto sym  = def->sym(); // a short Sym is stored inline, so its view must not outlive it
            auto name = sym.view().substr(self.size());
            auto dot  = name.find('.');
            auto mod  = dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
            res.annexes.emplace_back(std::string(mod), std::string(mod.empty() ? name : name.substr(dot + 1)), def);
        }
        for (auto dep : def->deps())
            if (dep && res.live.emplace(dep).second) todo.emplace_back(dep);
    }
    std::ranges::sort(res.annexes, {}, [](const auto& a) { return a.def->flags(); });
    return res;
}

} // namespace

/*
 * Def
 */

/// Def::Dump::Expr: one Def, one line, no analysis - and the backend of `std::formatter` for every Def pointer.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    auto _       = def->world().freeze();
    auto printer = dump::Printer(true);
    return os << printer.op(def);
}

std::ostream& Def::stream(std::ostream& os, Dump mode) const {
    auto _ = world().freeze();
    if (mode == Dump::Expr) return os << this << std::endl;
    dump::Dumper(os, mode, world().flags().mim_typed_let).dump(this);
    return os;
}

void Def::dump() const { std::cout << this << std::endl; }
void Def::dump(Dump mode) const { stream(std::cout, mode); }

void Def::write(Dump mode, const char* file) const {
    auto ofs = std::ofstream(file);
    stream(ofs, mode);
}

void Def::write(Dump mode) const {
    auto file = dump::Printer(true).id(this) + ".mim"s;
    write(mode, file.c_str());
}

/*
 * World
 */

void World::dump(std::ostream& os) {
    auto _        = freeze();
    auto old_gid  = curr_gid();
    auto reserved = fe::Vector<std::string>{"return"};
    auto srcs     = dump_imports(os, driver(), reserved);

    if (!driver().imports().entries().empty() && externals().size() != 0) os << '\n';

    for (auto mut : externals().muts())
        reserved.emplace_back(mut->sym().str());

    auto self            = file_stem(*this) + ".";
    auto [live, annexes] = reach(*this, self);
    for (const auto& annex : annexes)
        reserved.emplace_back(annex.mod.empty() ? annex.name : annex.mod);

    auto dumper = dump::Dumper(os, Def::Dump::All, flags().mim_typed_let, std::move(srcs));
    dumper.plain(reserved);
    dumper.live(live);
    dumper.generics(generics(*this, self));
    if (!annexes.empty()) dumper.dump_annexes(std::move(self), annexes);
    dumper.dump(externals().mutate());

    assertf(old_gid == curr_gid(), "new nodes created during dump. old_gid: {}; curr_gid: {}", old_gid, curr_gid());
}

void World::dump() { dump(std::cout); }

void World::debug_dump() {
    if (log().level() >= fe::Log::Level::Debug) dump(log().ostream());
}

void World::write(const char* file) {
    auto ofs = std::ofstream(file);
    dump(ofs);
}

void World::write() {
    auto file = file_stem(*this) + ".mim"s;
    write(file.c_str());
}

} // namespace mim
