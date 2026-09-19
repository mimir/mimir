/**

Lexes Mim and highlights the snippets in the documentation.
Doxygen has no Mim parser and discards the language of a fenced code block, so a snippet is marked up with a `mim-code` wrapper and coloured here.
The playground stages this file as `mim-code.js` and maps the kinds of `next` itself.

*/

class MimCode {
    static KEYWORD = new Set(["cn", "end", "fn", "inj", "lm", "match", "ret", "when", "where", "with", "λ"])
    /// `C_DECL` of `src/mim/ast/family.h` plus the modifiers of `Parser::parse_modifiers`.
    static DECL = new Set(["and", "anx", "as", "axm", "con", "extern", "fun", "import", "lam", "let", "mod", "norm",
                           "plugin", "priv", "pub", "rec", "rule", "use"])
    static TYPE = new Set(["Bool", "Cn", "Fn", "I1", "I8", "I16", "I32", "I64", "Idx", "Nat", "Rule", "Type", "Univ",
                           "i1", "i8", "i16", "i32", "i64"])
    static LITERAL = new Set(["bot", "ff", "top", "tt", "⊥", "⊤"])
    static SPECIAL = new Set(["_", "return"])
    static TOKEN = /(\/\/.*)|(\/\*)|("(?:\\.|[^"\\])*")|('(?:\\.|[^'\\])*')|([_a-zA-Z][_0-9a-zA-Z]*|[λ⊥⊤])|(\d(?:[\w.\u2080-\u2089]|'(?=\w))*)|([«»‹›→←Π∀])|(\s+|[^])/uy
    /// Doxygen's own token classes; a kind without one is left unhighlighted.
    static CLASS = {comment: "comment", string: "stringliteral", number: "mim-literal", keyword: "keyword",
                    decl: "keywordflow", type: "keywordtype", literal: "mim-literal", special: "mim-special"}

    static init() {
        $(function() {
            for (const fragment of document.querySelectorAll(".mim-code div.fragment")) {
                if (fragment.querySelector("span")) continue // Doxygen highlighted this one itself.

                const state = {comment: false}
                for (const line of fragment.querySelectorAll("div.line")) line.innerHTML = MimCode.code(line.textContent, state)
            }
        })
    }

    static escape(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;")
    }

    static span(cls, text) {
        return `<span class="${cls}">${MimCode.escape(text)}</span>`
    }

    static word(text) {
        if (MimCode.KEYWORD.has(text)) return "keyword"
        if (MimCode.DECL.has(text)) return "decl"
        if (MimCode.TYPE.has(text)) return "type"
        if (MimCode.LITERAL.has(text)) return "literal"
        if (MimCode.SPECIAL.has(text)) return "special"
        return "name"
    }

    /// Ends the token at `pos`; `state.comment` carries an unterminated `/*` into the following lines.
    static next(text, pos, state) {
        if (!state.comment) {
            MimCode.TOKEN.lastIndex = pos
            const [all, comment, open, string, char, word, number, op] = MimCode.TOKEN.exec(text)
            if (!open) return {end: pos + all.length, kind: comment ? "comment"
                                                          : string || char ? "string"
                                                          : word ? MimCode.word(word)
                                                          : number ? "number"
                                                          : op ? "operator" : null}
            state.comment = true
        }

        const end = text.indexOf("*/", pos)
        state.comment = end < 0
        return {end: end < 0 ? text.length : end + 2, kind: "comment"}
    }

    static code(text, state) {
        let out = "", pos = 0

        while (pos < text.length) {
            const {end, kind} = MimCode.next(text, pos, state)
            const cls = MimCode.CLASS[kind]
            const token = text.slice(pos, end)
            out += cls ? MimCode.span(cls, token) : MimCode.escape(token)
            pos = end
        }
        return out
    }
}
