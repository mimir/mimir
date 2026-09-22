#pragma once

#include <string>

#include <fe/lexer.h>

#include "mim/driver.h"

#include "mim/ast/tok.h"

namespace mim::ast {

class Lexer : public fe::Lexer<3, Lexer> {
    using Super = fe::Lexer<3, Lexer>;

public:
    /// Creates a lexer to read `*.mim` files (see [Lexical Structure](@ref lex)).
    /// If @p md is not `nullptr`, a Markdown output will be generated.
    Lexer(Driver& driver, const fe::Src& src, std::ostream* md = nullptr)
        : Lexer(driver, src.buf(), &src, md) {}
    /// As above, but the Loc%ations of the Tok%s produced have no fe::Src to resolve against.
    Lexer(Driver& driver, std::string_view buf, std::ostream* md = nullptr)
        : Lexer(driver, buf, nullptr, md) {}

    Driver& driver() { return driver_; } ///< fe::Lexer's default diagnostics go to its Driver::error.
    Tok lex();

    /// Does @p str match the `id` production - the same rule Lexer::lex_id applies to the input?
    static bool is_id(std::string_view str);

    /// Inverse of Lexer::lex_char: renders @p str as the body of a Mim string literal.
    static std::string escape(std::string_view str);

private:
    Lexer(Driver&, std::string_view, const fe::Src*, std::ostream*);

    Tok tok(Tok::Tag tag) { return {loc_, tag}; }
    Sym sym() { return driver().sym(view()); }
    bool lex_id();
    char8_t lex_char();
    Tok lex_str();
    Tok lex_lit();
    void lex_digits(int base = 10);
    bool lex_exp(int base = 10);
    void eat_comments();

    /// Interns the string literal occupying `[begin, end)` of Lexer::buf_; @p esc resolves its escapes first.
    Sym sym_str(uint32_t begin, uint32_t end, bool esc);
    /// Resolves the escapes of @p body, which starts at byte @p begin of Lexer::buf_.
    std::string unquote(std::string_view body, uint32_t begin);

    /// @name Markdown
    /// Whatever is consumed lands in Lexer::md_ verbatim, wrapped in a `mim` code fence.
    /// A `///` line interrupts that fence and goes through as Markdown, its marker skipped.
    ///@{
    bool start_md() const { return ahead(0) == '/' && ahead(1) == '/' && ahead(2) == '/'; }
    void emit_md(bool start_of_file = false);
    size_t pos() const { return peek().begin.off; } ///< First byte not yet consumed.
    /// Writes @p str to Lexer::md_ while tracking the newlines the output ends with.
    /// Outside a fence a run of empty lines collapses into one, as a Markdown linter wants.
    void md_emit(std::string_view str) {
        if (!md_) return;
        for (auto c : str) {
            if (c == '\n' && !fenced_ && md_nls_ >= 2) continue;
            *md_ << c;
            md_nls_ = c == '\n' ? md_nls_ + 1 : 0;
        }
    }
    void md_blank() { md_emit("\n\n"); } ///< Makes the output end with an empty line, as a fence needs on either side.
    void md_flush() {
        md_emit(buf_.substr(md_pos_, pos() - md_pos_));
        md_pos_ = pos();
    }
    void md_skip() { md_pos_ = pos(); }
    /// The language tag switches on Mim syntax highlighting in the generated documentation.
    void md_open() {
        md_blank();
        md_emit("```mim\n");
        fenced_ = md_ != nullptr;
    }
    void md_close() {
        md_emit("```\n");
        fenced_ = false;
    }
    ///@}

    Driver& driver_;
    std::ostream* md_;
    size_t md_pos_ = 0;
    size_t md_nls_ = 2; ///< Newlines the Markdown output ends with; starts at 2, so it never begins with an empty line.
    bool fenced_   = false; ///< Is a code fence currently open?

    friend class fe::Lexer<3, Lexer>;
};

} // namespace mim::ast
