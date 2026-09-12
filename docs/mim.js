/**

Highlights the Mim snippets in the documentation.
Doxygen has no Mim parser and discards the language of a fenced code block, so a snippet is marked up with a `mim-code` wrapper and coloured here.

*/

class MimCode {
    static KEYWORD = new Set(["and", "anx", "axm", "cn", "con", "extern", "fn", "fun", "import", "lam", "let", "lm",
                              "mod", "norm", "plugin", "priv", "pub", "rec", "rule", "use", "λ"])
    static FLOW = new Set(["as", "end", "inj", "match", "ret", "when", "where", "with"])
    static TYPE = new Set(["Bool", "Cn", "Fn", "I1", "I8", "I16", "I32", "I64", "Idx", "Nat", "Rule", "Type", "Univ",
                           "i1", "i8", "i16", "i32", "i64"])
    static LITERAL = new Set(["bot", "ff", "top", "tt", "⊥", "⊤"])
    static SPECIAL = new Set(["_", "return"])
    static TOKEN = /^(\/\/.*)|^(\/\*)|^("(?:\\.|[^"\\])*")|^('(?:\\.|[^'\\])*')|^([_a-zA-Z][_0-9a-zA-Z]*|[λ⊥⊤])|^(\d(?:[\w.\u2080-\u2089]|'(?=\w))*)|^(\s+)|^([^])/u

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
        if (MimCode.KEYWORD.has(text)) return MimCode.span("keyword", text)
        if (MimCode.FLOW.has(text)) return MimCode.span("keywordflow", text)
        if (MimCode.TYPE.has(text)) return MimCode.span("keywordtype", text)
        if (MimCode.LITERAL.has(text)) return MimCode.span("mim-literal", text)
        if (MimCode.SPECIAL.has(text)) return MimCode.span("mim-special", text)
        return MimCode.escape(text)
    }

    /// `state.comment` carries an unterminated `/*` into the following lines of the same snippet.
    static code(text, state) {
        let out = "", rest = text

        while (rest) {
            if (state.comment) {
                const end = rest.indexOf("*/")
                out += MimCode.span("comment", end < 0 ? rest : rest.slice(0, end + 2))
                rest = end < 0 ? "" : rest.slice(end + 2)
                state.comment = end < 0
                continue
            }

            const [all, comment, open, string, char, word, number, space] = MimCode.TOKEN.exec(rest)
            if (comment)      out += MimCode.span("comment", comment)
            else if (open)    { state.comment = true; continue }
            else if (string)  out += MimCode.span("stringliteral", string)
            else if (char)    out += MimCode.span("stringliteral", char)
            else if (word)    out += MimCode.word(word)
            else if (number)  out += MimCode.span("mim-literal", number)
            else if (space)   out += space
            else              out += MimCode.escape(all)
            rest = rest.slice(all.length)
        }
        return out
    }
}
