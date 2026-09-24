#include <fstream>
#include <ostream>
#include <sstream>

#include "mim/axm.h"
#include "mim/driver.h"
#include "mim/world.h"

#include "mim/ast/ast.h"
#include "mim/ast/lexer.h"

#include "dump/ascii.h"
#include "dump/layout.h"
#include "dump/names.h"
#include "dump/unparse.h"

namespace mim {

namespace {

using Dump = Def::Dump;

/// The name of the file a World is dumped to - and hence the module its own annexes live in.
std::string file_stem(World& world) { return (world.name() ? world.name() : world.sym("_default")).str(); }

void emit(std::ostream& os, World& world, const ast::Node* node) {
    auto tab = fe::Tab::spaces();
    if (!world.flags().ascii) return node->stream(tab, os);
    auto ss = std::ostringstream();
    node->stream(tab, ss);
    os << dump::ascii(ss.str());
}

} // namespace

/*
 * Def
 */

/// Def::Dump::Expr: one Def, one line, no analysis - and the backend of `std::formatter` for every Def pointer.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    auto& world = def->world();
    auto _      = world.freeze();
    auto ast    = ast::AST(world, 4096);
    auto layout = dump::Layout(world, {.mode = Dump::Expr});
    emit(os, world, dump::Unparser(ast, layout).expr(def).get());
    return os;
}

std::ostream& Def::stream(std::ostream& os, Dump mode) const {
    auto _ = world().freeze();
    if (mode == Dump::Expr) return os << this << std::endl;

    auto old_gid = world().curr_gid();
    auto ast     = ast::AST(world());
    auto layout  = dump::Layout(world(), {.mode = mode, .typed_let = world().flags().mim_typed_let});
    layout.add_root(this);
    auto reserved = std::array{world().sym("return")};
    auto names    = dump::Names(world().driver(), dump::Names::Policy::Unique, reserved);
    layout.finish(names);

    auto unparser = dump::Unparser(ast, layout);
    if (auto& top = layout.top(); top.tail) {
        emit(os, world(), unparser.block(top).get());
        os << '\n';
    } else {
        emit(os, world(), ast.ptr<ast::File>(Loc(), ast.scope(), ast.copy(unparser.decls(top))).get());
    }
    assertf(old_gid == world().curr_gid(), "new nodes created during dump. old_gid: {}; curr_gid: {}", old_gid,
            world().curr_gid());
    return os;
}

void Def::dump() const { std::cout << this << std::endl; }
void Def::dump(Dump mode) const { stream(std::cout, mode); }

void Def::write(Dump mode, const char* file) const {
    auto ofs = std::ofstream(file);
    stream(ofs, mode);
}

void Def::write(Dump mode) const {
    auto file = dump::Names::fallback(this) + ".mim";
    write(mode, file.c_str());
}

/*
 * World
 */

void World::dump(std::ostream& os) {
    auto _       = freeze();
    auto old_gid = curr_gid();
    auto srcs    = absl::flat_hash_set<const fe::Src*>();
    for (const auto& import : driver().imports())
        srcs.emplace(import.src);

    // Names the dump refers to verbatim and hence must not pick for anything else.
    auto reserved = fe::Vector<Sym>{sym("return")};
    for (const auto& import : driver().imports())
        if (auto module = import.src->path().stem().string(); ast::Lexer::is_id(module))
            reserved.emplace_back(sym(module));
    for (auto mut : externals().muts())
        reserved.emplace_back(mut->sym());

    // An `import` declares its own annexes; those of the dumped file itself only exist if the dump declares them.
    // Only a loaded plugin registers its annexes, so find them by what the externals reach.
    auto self = file_stem(*this) + ".";
    auto axms = fe::Vector<std::pair<std::string, const Axm*>>();
    auto todo = DefVec(externals().muts().begin(), externals().muts().end());
    auto seen = DefSet(todo.begin(), todo.end());
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if (auto axm = def->isa<Axm>(); axm && axm->sym().view().starts_with(self)) {
            auto name = axm->sym().str().substr(self.size());
            reserved.emplace_back(sym(name.substr(0, name.find('.'))));
            axms.emplace_back(std::move(name), axm);
        }
        for (auto dep : def->deps())
            if (dep && seen.emplace(dep).second) todo.emplace_back(dep);
    }
    std::ranges::sort(axms, {}, [](const auto& p) { return p.second->flags(); });

    // The local dump keeps every mutable to itself: no Nest that a broken program could trip over.
    auto mode   = flags().mim_local ? Dump::Local : Dump::All;
    auto ast    = ast::AST(*this);
    auto layout = dump::Layout(
        *this, {.mode = mode, .typed_let = flags().mim_typed_let, .skip = std::move(srcs), .self = self});
    layout.add_axms(axms);
    // An `import` declares its own externals; what they reach is declared here only if our own code reaches it.
    for (auto mut : externals().muts())
        if (!layout.opts().skip.contains(mut->loc().src)) layout.add_root(mut);
    auto names = dump::Names(driver(), dump::Names::Policy::Plain, reserved);
    layout.finish(names);

    auto unparser = dump::Unparser(ast, layout);
    auto decls    = ast::Ptrs<ast::ValDecl>();
    for (const auto& import : driver().imports())
        decls.emplace_back(unparser.use(import));
    decls.append_range(unparser.decls(layout.top()));
    emit(os, *this, ast.ptr<ast::File>(Loc(), ast.scope(), ast.copy(decls)).get());

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
    auto file = file_stem(*this) + ".mim";
    write(file.c_str());
}

} // namespace mim
