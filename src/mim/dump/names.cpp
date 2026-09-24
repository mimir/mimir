#include "names.h"

#include "mim/driver.h"

#include "mim/ast/lexer.h"

namespace mim::dump {

Names::Names(Driver& driver, Policy policy, std::span<const Sym> reserved)
    : driver_(driver)
    , policy_(policy) {
    taken_.insert(reserved.begin(), reserved.end());
}

Sym Names::pick(const Def* def) {
    if (policy_ == Policy::Unique && def) return driver_.sym(def->unique_name());

    auto sym   = def ? def->sym() : Sym();
    auto is_id = sym && sym != '_' && ast::Lexer::is_id(sym.view()) && !driver_.keys().find(sym);
    auto base  = is_id ? sym : driver_.sym("_");
    if (is_id && taken_.emplace(base).second) return base;

    // A numbered variant of an identifier is an identifier and never a keyword.
    for (auto& i = counters_[base];;) {
        auto name = driver_.sym(std::format("{}{}{}", base.view(), is_id ? "_" : "", ++i));
        if (taken_.emplace(name).second) return name;
    }
}

std::string Names::fallback(const Def* def) {
    if (auto sym = def->sym(); sym && sym != '_' && PlainNames::claim(def->world().driver(), sym, def->gid()))
        return sym.str();
    return def->unique_name();
}

} // namespace mim::dump
