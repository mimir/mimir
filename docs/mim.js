/**

Lexes Mim; see `docs/code.js` for the machinery.
Each snippet also gets a button that opens it in the playground via its `?src=` parameter.

*/

class MimCode extends Code {
    static WORDS = {
        keyword: new Set(["cn", "end", "fn", "inj", "lm", "match", "ret", "when", "where", "with", "λ"]),
        /// `C_DECL` of `src/mim/ast/family.h` plus the modifiers of `Parser::parse_modifiers`.
        decl: new Set(["and", "anx", "as", "axm", "con", "extern", "fun", "import", "lam", "let", "mod", "norm",
                       "plugin", "priv", "pub", "rec", "rule", "use"]),
        type: new Set(["Bool", "Cn", "Fn", "I1", "I8", "I16", "I32", "I64", "Idx", "Nat", "Rule", "Type", "Univ"]),
        literal: new Set(["bot", "ff", "i1", "i8", "i16", "i32", "i64", "top", "tt", "⊥", "⊤"]),
        special: new Set(["_", "return"])
    }
    static PLAYGROUND = "https://mimir.github.io/playground/"
    static RUN_TITLE = "Run in the playground"
    static RUN_ICON = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24"><path d="M5.5 3.5 20.5 12 5.5 20.5Z"/></svg>`
    static TOKEN = /(\/\/.*)|(\/\*)|("(?:\\.|[^"\\])*")|('(?:\\.|[^'\\])*')|([_a-zA-Z][_0-9a-zA-Z]*|[λ⊥⊤])|(\d(?:[\w.\u2080-\u2089]|'(?=\w))*)|([«»‹›→←Π∀])|(\s+|[^])/uy
    static KINDS = ["comment", "open", "string", "string", "word", "number", "operator", null]
    static CLASS = {...Code.CLASS, special: "token-special"}

    /// `state.comment` carries an unterminated `/*` into the following lines.
    static next(text, pos, state) {
        if (!state.comment) {
            const token = super.next(text, pos, state)
            if (token.kind !== "open") return token
            state.comment = true
        }

        const end = text.indexOf("*/", pos)
        state.comment = end < 0
        return {end: end < 0 ? text.length : end + 2, kind: "comment"}
    }

    static decorate(fragment) {
        this.run(fragment)
        super.decorate(fragment)
    }

    /// Doxygen renders a blank line as a lone space.
    static source(fragment) {
        return [...fragment.querySelectorAll("div.line")].map(line => line.textContent.replace(/\s+$/, "")).join("\n")
    }

    /// The copy button already wraps every fragment — but only in a browser that has a clipboard.
    static wrapper(fragment) {
        const parent = fragment.parentNode
        if (parent.classList.contains("doxygen-awesome-fragment-wrapper")) return parent

        const wrapper = document.createElement("div")
        wrapper.className = "doxygen-awesome-fragment-wrapper"
        parent.replaceChild(wrapper, fragment)
        wrapper.appendChild(fragment)
        return wrapper
    }

    static run(fragment) {
        const button = document.createElement("a")
        button.className = "mim-run"
        button.href = `${this.PLAYGROUND}?src=${encodeURIComponent(this.source(fragment))}`
        button.target = "_blank"
        button.rel = "noopener"
        button.title = this.RUN_TITLE
        button.innerHTML = this.RUN_ICON
        this.wrapper(fragment).appendChild(button)
    }
}

MimCode.register("mim")
