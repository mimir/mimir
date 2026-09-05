#pragma once

#include <absl/container/flat_hash_map.h>
#include <fe/lexer.h>

#include "mim/ast/tok.h"

namespace mim::ast {

class Lexer : public fe::Lexer<3, Lexer> {
    using Super = fe::Lexer<3, Lexer>;

public:
    /// Creates a lexer to read `*.mim` files (see [Lexical Structure](@ref lex)).
    /// If @p md is not `nullptr`, a Markdown output will be generated.
    Lexer(fe::Driver& driver, const fe::Src& src, std::ostream* md = nullptr)
        : Lexer(driver, src.buf(), &src, md) {}
    /// As above, but the Loc%ations of the Tok%s produced have no fe::Src to resolve against.
    Lexer(fe::Driver& driver, std::string_view buf, std::ostream* md = nullptr)
        : Lexer(driver, buf, nullptr, md) {}

    fe::Driver& driver() { return driver_; } ///< fe::Lexer's default diagnostics go to its Driver::error.
    Tok lex();

private:
    Lexer(fe::Driver&, std::string_view, const fe::Src*, std::ostream*);

    char32_t next() {
        auto res = Super::next();
        if (md_ && out_) {
            if (res == fe::utf8::EoF) {
                *md_ << "\n```\n";
                out_ = false;
            } else if (res) {
                bool success = fe::utf8::encode(*md_, res);
                assert_unused(success);
            }
        }
        return res;
    }

    Tok tok(Tok::Tag tag) { return {loc_, tag}; }
    Sym sym();
    bool lex_id(bool dots = false);
    char8_t lex_char();
    std::optional<Tok> parse_lit();
    void parse_digits(int base = 10);
    bool parse_exp(int base = 10);
    void eat_comments();
    bool start_md() const { return ahead(0) == '/' && ahead(1) == '/' && ahead(2) == '/'; }
    void emit_md(bool start_of_file = false);
    void md_fence() {
        if (md_) *md_ << "```\n";
    }

    fe::Driver& driver_;
    std::ostream* md_;
    bool out_ = true;
    fe::SymMap<Tok::Tag> keywords_;

    friend class fe::Lexer<3, Lexer>;
};

} // namespace mim::ast
