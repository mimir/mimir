/**

The lexer the code blocks of the documentation share.
Doxygen discards the language of a fenced code block, so `docs/fences.py` wraps a listing in a `<lang>-code` div and the `Code` subclass registered for `<lang>` colours it here.
The playground stages these files and drives the very same lexers.

*/

class Code {
    /// Sticky alternation a subclass provides; the `i`th capture yields `KINDS[i]`.
    static TOKEN = null
    static KINDS = null
    /// Probed in order for a `word`; the name of the set holding it becomes its kind.
    static WORDS = {}
    /// `docs/code.css` colours these; a kind without one is left unhighlighted.
    static CLASS = {comment: "token-comment", string: "token-string", number: "token-literal", keyword: "token-keyword",
                    decl: "token-decl", type: "token-type", literal: "token-literal"}
    static LANGS = new Map()

    static register(lang) {
        Code.LANGS.set(lang, this)
    }

    static init() {
        $(function() {
            for (const [lang, code] of Code.LANGS)
                for (const fragment of document.querySelectorAll(`.${lang}-code div.fragment`)) code.decorate(fragment)
        })
    }

    static decorate(fragment) {
        if (fragment.querySelector("span")) return // Doxygen highlighted this one itself.

        const state = {}
        for (const line of fragment.querySelectorAll("div.line")) line.innerHTML = this.code(line.textContent, state)
    }

    static escape(text) {
        return /[&<]/.test(text) ? text.replace(/&/g, "&amp;").replace(/</g, "&lt;") : text
    }

    static span(cls, text) {
        return `<span class="${cls}">${this.escape(text)}</span>`
    }

    /// Ends the token at `pos`.
    static next(text, pos, state) {
        this.TOKEN.lastIndex = pos
        const match = this.TOKEN.exec(text)
        const group = match.findIndex((capture, i) => i > 0 && capture !== undefined)
        return {end: pos + match[0].length, kind: this.kind(this.KINDS[group - 1], match[0])}
    }

    /// Refines what the matching capture yielded, e.g. a word into a keyword.
    static kind(kind, text) {
        if (kind !== "word") return kind
        for (const word in this.WORDS) if (this.WORDS[word].has(text)) return word
        return "name"
    }

    static code(text, state = {}) {
        let out = "", pos = 0

        while (pos < text.length) {
            const {end, kind} = this.next(text, pos, state)
            out += this.emit(kind, text.slice(pos, end))
            pos = end
        }
        return out
    }

    static emit(kind, token) {
        const cls = this.CLASS[kind]
        return cls ? this.span(cls, token) : this.escape(token)
    }
}
