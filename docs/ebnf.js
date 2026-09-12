/**

Colours the EBNF blocks of the language reference.
Doxygen discards the language of a fenced code block, so a grammar block is marked up with an `ebnf-code` wrapper and a terminal list with an `ebnf-terminals` one.

*/

class MimEbnf {
    static LEXICAL = new Set(["I", "L", "C", "S", "X_n"])
    static HEAD = /^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*)(::=)/
    static TOKEN = /("(?:\\.|[^"\\])*")|(\[(?:"[^"]*"|[^\]\s])*\])|(::=)|([A-Za-z_][A-Za-z0-9_]*)|([()|\[\]*+?,])|(\s+)|([^])/gu

    static init() {
        $(function() {
            MimEbnf.highlight(".ebnf-terminals div.fragment", MimEbnf.terminals)
            MimEbnf.highlight(".ebnf-code div.fragment", MimEbnf.rule)
            document.addEventListener("click", MimEbnf.trace)
        })
    }

    static highlight(selector, colour) {
        for (const fragment of document.querySelectorAll(selector)) {
            if (fragment.querySelector("span")) continue // Doxygen highlighted this one itself.

            for (const line of fragment.querySelectorAll("div.line")) line.innerHTML = colour(line.textContent)
        }
    }

    static escape(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;")
    }

    static symbol(name, def = "") {
        const kind = MimEbnf.LEXICAL.has(name) ? "ebnf-token" : "ebnf-rule"
        return `<span class="${kind}${def}" data-ebnf-sym="${name}">${MimEbnf.escape(name)}</span>`
    }

    static terminal(text) {
        const body = MimEbnf.escape(text.slice(1, -1))
        return `<span class="ebnf-terminal"><span class="ebnf-quote">"</span>${body}<span class="ebnf-quote">"</span></span>`
    }

    static klass(text) {
        return `<span class="ebnf-meta">[</span><span class="ebnf-class">${MimEbnf.escape(text.slice(1, -1))}</span><span class="ebnf-meta">]</span>`
    }

    static terminals(text) {
        return MimEbnf.escape(text).replace(/\S+/g, token => `<span class="ebnf-terminal">${token}</span>`)
    }

    static rule(text) {
        let out = "", rest = text
        const head = MimEbnf.HEAD.exec(text)

        if (head) {
            out = head[1] + MimEbnf.symbol(head[2], " ebnf-def") + head[3] + `<span class="ebnf-meta">::=</span>`
            rest = text.slice(head[0].length)
        }

        MimEbnf.TOKEN.lastIndex = 0
        for (let match; (match = MimEbnf.TOKEN.exec(rest));) {
            const [all, terminal, klass, def, id, meta, space] = match
            if (terminal)        out += MimEbnf.terminal(terminal)
            else if (klass)      out += MimEbnf.klass(klass)
            else if (def)        out += `<span class="ebnf-meta">::=</span>`
            else if (id)         out += MimEbnf.symbol(id)
            else if (meta)       out += `<span class="ebnf-meta">${MimEbnf.escape(meta)}</span>`
            else if (space)      out += space
            else                 out += MimEbnf.escape(all)
        }
        return out
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
