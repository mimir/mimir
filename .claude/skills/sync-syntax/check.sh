#!/bin/sh
# Diffs the keyword and terminal tables of `include/mim/ast/tok.h` against the downstream targets.
# usage: check.sh [mimir-root]   (siblings: $VIM_MIM, $TS_MIM, $VSC_MIM, default ../vim-mim, ../tree-sitter-mim, ../tree-sitter-mim-vscode)

set -uf # `*` is a terminal, not a glob
MIMIR=${1:-$(git rev-parse --show-toplevel 2>/dev/null || echo .)}
VIM_MIM=${VIM_MIM:-$MIMIR/../vim-mim}
TS_MIM=${TS_MIM:-$MIMIR/../tree-sitter-mim}
VSC_MIM=${VSC_MIM:-$MIMIR/../tree-sitter-mim-vscode}

TOK=$MIMIR/include/mim/ast/tok.h
tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT

# every spelling the keyword table yields: MIM_KEY plus the MIM_SUBST aliases
{ grep -oP 'm\(K_\w+,\s*"\K[^"]+' "$TOK"; grep -oP '^\s*m\("\K[^"]+' "$TOK"; } | sort -u > "$tmp/keys"
# the punctuation tokens; only the L_/M_/EoF entries of MIM_TOK use a `<...>` placeholder
grep -oP 'm\((?:T|D)_\w+,\s*"\K[^"]+' "$TOK" | sort -u > "$tmp/toks"

REF=$tmp/keys
report() { # report <label> <words-file> <missing|both>; diffs $REF against <words-file>
    comm -23 "$REF" "$2" > "$tmp/gone"
    comm -13 "$REF" "$2" > "$tmp/new"
    printf '\n== %s ==\n' "$1"
    [ -s "$tmp/gone" ] && printf 'missing: %s\n' "$(tr '\n' ' ' < "$tmp/gone")"
    [ "$3" = both ] && [ -s "$tmp/new" ] && printf 'stale:   %s\n' "$(tr '\n' ' ' < "$tmp/new")"
    [ -s "$tmp/gone" ] || { [ "$3" = both ] && [ -s "$tmp/new" ]; } || printf 'in sync\n'
    return 0
}

# `lm`/`bot`/`top` are listed as secondary terminals rather than in the keyword block
{ sed -n '/^#### Keywords/,/^#### /p' "$MIMIR/docs/langref.md" | sed -n '/```text/,/```/p' | grep -v '```'
  sed -n '/^#### Secondary Terminals/,/^#### /p' "$MIMIR/docs/langref.md" | grep -oP '`\K[^`]+'
} | tr -s ' \t' '\n' | grep -xE '[A-Za-z_][A-Za-z0-9_]*' | sort -u > "$tmp/langref"
report 'docs/langref.md - Keywords + Secondary Terminals' "$tmp/langref" both

# `_`, `return`, `λ`, `⊥`, `⊤` sit in WORDS without being keywords
sed -n '/static WORDS/,/^    }/p' "$MIMIR/docs/mim.js" | grep -oP '"\K[^"]+' \
    | grep -xE '[A-Za-z_][A-Za-z0-9_]*' | grep -vxE '_|return' | sort -u > "$tmp/mimjs"
report 'docs/mim.js - WORDS' "$tmp/mimjs" both

grep -oP '^\s*syn keyword mim\w+\s+\K.*' "$VIM_MIM/syntax/mim.vim" | tr -s ' \t' '\n' \
    | grep . | grep -vxE 'contained|TODO|FIXME|XXX|NOTE|return' | sort -u > "$tmp/vim"
report 'vim-mim/syntax/mim.vim - syn keyword' "$tmp/vim" both

# grammar.js also quotes field names, so only the missing direction means anything
grep -oP '"\K[a-zA-Z_]\w*(?=")' "$TS_MIM/grammar.js" | sort -u > "$tmp/ts"
report 'tree-sitter-mim/grammar.js' "$tmp/ts" missing

REF=$tmp/toks
sed -n '/^#### Primary Terminals/,/^#### /p' "$MIMIR/docs/langref.md" | sed -n '/```text/,/```/p' \
    | grep -v '```' | tr -s ' \t' '\n' | grep . | grep -vx '<eof>' | sort -u > "$tmp/prim"
report 'docs/langref.md - Primary Terminals' "$tmp/prim" both

# a mapped capture that highlights.scm no longer emits is dead config
grep -oP '@\K[\w.]+' "$TS_MIM/queries/highlights.scm" | sort -u > "$tmp/caps"
python3 -c 'import json,sys; [print(k) for c in json.load(open(sys.argv[1])) if c["lang"] == "mim" for k in c.get("semanticTokenTypeMappings", {})]' \
    "$VSC_MIM/language-configs.json" | sort -u > "$tmp/mapped"
printf '\n== tree-sitter-mim-vscode/language-configs.json - mim captures ==\n'
comm -13 "$tmp/caps" "$tmp/mapped" > "$tmp/new"
[ -s "$tmp/new" ] && printf 'stale:   %s\n' "$(tr '\n' ' ' < "$tmp/new")" || printf 'in sync\n'
