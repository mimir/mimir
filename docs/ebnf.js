/**

Lexes the EBNF grammar and the terminal lists of the language reference; see `docs/code.js` for the machinery.
Clicking a symbol traces its every occurrence.

*/

class EbnfCode extends Code {
    static LEXICAL = new Set(["I", "L", "C", "S", "X_n"])
    /// Stops short of the `::=` so that the shared lexer emits it.
    static HEAD = /^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*)(?=::=)/
    static TOKEN = /("(?:\\.|[^"\\])*")|(\/\/.*)|(\[(?:"[^"]*"|[^\]\s])*\])|(::=)|([A-Za-z_][A-Za-z0-9_]*)|([()|\[\]*+?,])|(\s+|[^])/uy
    static KINDS = ["terminal", "comment", "class", "meta", "symbol", "meta", null]
    static CLASS = {comment: "comment", meta: "ebnf-meta"}

    static code(text, state = {}) {
        const head = this.HEAD.exec(text)
        if (!head) return super.code(text, state)

        return head[1] + this.symbol(head[2], " ebnf-def") + head[3] + super.code(text.slice(head[0].length), state)
    }

    static emit(kind, token) {
        if (kind === "terminal") return this.terminal(token)
        if (kind === "class") return this.klass(token)
        if (kind === "symbol") return this.symbol(token)
        return super.emit(kind, token)
    }

    static symbol(name, def = "") {
        const kind = this.LEXICAL.has(name) ? "ebnf-token" : "ebnf-rule"
        return `<span class="${kind}${def}" data-ebnf-sym="${name}">${this.escape(name)}</span>`
    }

    static terminal(text) {
        const body = this.escape(text.slice(1, -1))
        return `<span class="ebnf-terminal"><span class="ebnf-quote">"</span>${body}<span class="ebnf-quote">"</span></span>`
    }

    static klass(text) {
        return this.span("ebnf-meta", "[") + this.span("ebnf-class", text.slice(1, -1)) + this.span("ebnf-meta", "]")
    }

    static trace(event) {
        const target = event.target.closest("[data-ebnf-sym]")
        const on = target && !target.classList.contains("ebnf-trace")

        for (const span of document.querySelectorAll(".ebnf-trace")) span.classList.remove("ebnf-trace")
        if (on)
            for (const span of document.querySelectorAll(`[data-ebnf-sym="${target.dataset.ebnfSym}"]`))
                span.classList.add("ebnf-trace")
    }
}

/// A terminal list is just words.
class TerminalsCode extends Code {
    static TOKEN = /(\S+)|(\s+)/uy
    static KINDS = ["terminal", null]
    static CLASS = {terminal: "ebnf-terminal"}
}

EbnfCode.register("ebnf")
TerminalsCode.register("terminals")
$(() => { if (document.querySelector(".ebnf-code")) document.addEventListener("click", EbnfCode.trace) })
