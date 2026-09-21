---
name: sync-syntax
description: Propagate a change to Mim's surface syntax from the lexer/parser to the five downstream consumers - docs/langref.md, lit/docs/tutorial.mim, docs/mim.js, the vim-mim plugin, and the tree-sitter-mim grammar. Use after touching include/mim/ast/{tok,lexer,parser}.h or src/mim/ast/{lexer,parser}.cpp or src/mim/ast/family.h, and whenever asked to check or re-sync those targets.
---

# sync-syntax

The lexer and the parser define Mim's surface syntax; five other places merely describe it and silently rot when they are not updated in the same breath.

## Ground truth

| File | What it settles |
| ---- | --------------- |
| `include/mim/ast/tok.h` | `MIM_KEY` (keywords), `MIM_SUBST` (`lm`/`bot`/`top`), `MIM_TOK` (every punctuation spelling), `MIM_INFIX_SUGAR`/`MIM_INFIX_CORE`, `MIM_PREC` (the precedence ladder and its associativity) |
| `src/mim/ast/lexer.cpp` | which byte sequence yields which tag (`lex`), the literal grammar (`lex_lit`, `lex_digits`, `lex_exp`), escapes (`lex_char`, `unquote`), comments and `///` Markdown |
| `src/mim/ast/parser.cpp` | the productions and the `Prec` bound each `parse_expr`/`parse_ptrn` passes |
| `src/mim/ast/family.h` | which tags start which production (`C_EXPR`, `C_DECL`, `C_LAM`, …) |

Start from `git diff` over those files - the diff, not the current state, is what tells you which of the five targets are in play.

## Step 1: run the checker

```sh
.claude/skills/sync-syntax/check.sh
```

It diffs the keyword and terminal tables of `tok.h` against the four targets that carry a flat word list and reports what is `missing` (in `tok.h`, not in the target) or `stale` (the other way round).
It has nothing to say about `lit/docs/tutorial.mim`, and it only sees flat word lists - a new *production*, a changed *precedence*, or a new *literal form* passes it silently too.
All of that needs the manual pass below.
`VIM_MIM` and `TS_MIM` override the sibling checkouts (`../vim-mim`, `../tree-sitter-mim`).

## Step 2: what a change touches

| Change | langref | tutorial | mim.js | vim-mim | tree-sitter-mim |
| ------ | :-----: | :------: | :----: | :-----: | :-------------: |
| keyword added/removed/renamed | ✓ | ✓ | ✓ | ✓ | ✓ |
| new punctuation token or Unicode spelling | ✓ | ✓ | ✓ | ✓ | ✓ |
| literal syntax | ✓ | ✓ | ✓ | ✓ | ✓ |
| new/changed production | ✓ | ✓ | - | - | ✓ |
| comment syntax | ✓ | ✓ | ✓ | ✓ | ✓ |
| precedence or associativity | ✓ | - | - | - | ✓ |
| error message or diagnostic only | - | - | - | - | - |

A construct that was *removed or renamed* reaches the tutorial no matter which row it sits in: the file is a lit test, so a stale line breaks the build rather than going unnoticed.

## Step 3: the targets

### `docs/langref.md`

The normative reference; the other four follow it, so do this one first.

- *Primary Terminals* and *Secondary Terminals* mirror `MIM_TOK` and `MIM_SUBST`.
- *Keywords* mirrors `MIM_KEY`; the predefined-alias table below it mirrors what `bind.cpp` expands.
- *Lexical Terminals* is the EBNF for `I`, `L`, `X_n`, `C`, `S` and the `bin`/`dec`/`id`/`op`/`esc` shorthands - keep it matching `lex_lit`/`lex_char` literally.
  A *new* lexical terminal name also goes into `LEXICAL` in `docs/ebnf.js`, or it renders as a nonterminal.
- The *Grammar* sections carry one EBNF block per construct plus a bullet per rule; a new production needs both.
- The *Precedence* table mirrors `MIM_PREC` top to bottom, including which levels are pseudo levels.
- *Normalizations* only changes when a normalizer does.

### `lit/docs/tutorial.mim`

The tour: **every construct langref defines must show up here**, in a section following langref's own order.
It is a lit test, not prose - each claim is a static `refly` assertion, so the file compiling at all is what tests it - and it is the playground's landing example, so it is also the first Mim anyone reads.

- Show the new construct where it belongs, with a one-line `//` comment saying what it demonstrates.
  Match the surrounding density: one or two lines per construct, not a paragraph.
- Assert wherever there is something to assert.
  `refly.struc.e (a, b)` states that two *terms* are the same node; where two *types* coincide, a `lam` whose parameter type and return type differ is the assertion - as in `sigma_is_array`, `erased`, and `fuse`.
  A construct with nothing to compare against just has to typecheck.
- Both RUN lines matter: the second is `%mim -p ll %s`, so new code must also survive the ll backend.
  Keep anything the backend chokes on - a runtime-size pack, say - out of an `extern` the emitter reaches.
- Renaming or removing a construct breaks this file; fix it here rather than deleting the line, or the tour loses a section.

### `docs/mim.js`

The highlighter for `mim`-fenced blocks in the docs *and* for the playground - `web/CMakeLists.txt` stages this very file as `mim-code.js`, so there is nothing separate to update under `web/`.

- `WORDS` is probed in order and the set's *name* becomes the token kind, so put a new word in the right one: `keyword`, `decl` (that is `C_DECL` plus the `parse_modifiers` modifiers), `type`, `literal`, `special`.
- `TOKEN` is a sticky alternation whose capture *i* yields `KINDS[i]`; a new Unicode operator joins the `[«»‹›→←Π∀]` class and a new word-like character the identifier class.
- `CLASS` maps a kind to a Doxygen CSS class; a kind with no entry is left unhighlighted.
- The machinery lives in `docs/code.js` - read it before adding a kind.

### `../vim-mim`

- `syntax/mim.vim`: the `syn keyword` lines, `mimType`/`mimConstant` for the Unicode spellings, the `mimNumber`/`mimFloat`/`mimIndex` patterns, and `mimOperator`/`mimDelimiter`.
  Multi-character operators must stay *after* the single-character ones (later item wins at equal position) and `mimComment` must stay before the `/` operator.
- `ftplugin/mim.vim`: a token that has *only* a Unicode spelling needs an `iabbrev` **and** a matching `iunabbrev` in `b:undo_ftplugin`.
- `README.md`: the abbreviation table, and the sentence listing which symbols have ASCII spellings.

### `../tree-sitter-mim`

Read `../tree-sitter-mim/CLAUDE.md` first - it is the authority for that repo and explains the `PREC` mirror, the pattern/telescope split, and why `*`/`+`/`-` are handled structurally.

- `grammar.js` is the only hand-written grammar source; `src/` is generated **and committed**, so `tree-sitter generate` is part of the change.
- `queries/highlights.scm` (plus `injections.scm`, `folds.scm`) are the Neovim queries; renaming a grammar node breaks them and the external Helix ones.
- The corpus under `test/corpus/` is small - the real regression suite is this repo's `.mim` files.

## Step 4: verify

```sh
cmake --build build -j16                     # tok.h/lexer/parser still compile
cmake --build build --target mim_lit_tests   # re-stage lit before running it
cmake --build build --target lit             # tutorial.mim asserts the whole tour
cd lit && ../scripts/probe.sh tutorial.mim   # … or just that one, while iterating on it
cmake --build build --target docs            # langref + mim.js land in the doxygen site
.claude/skills/sync-syntax/check.sh          # tables agree again
```

```sh
cd ../tree-sitter-mim
tree-sitter generate && tree-sitter test
tree-sitter parse -q $(find ../mimir/lit ../mimir/src/mim/plug -name '*.mim')
# expected: only lit/error/{insert_no_index,missing_name,path_member,
#           unanchored_delim,unterminated_string,where_semicolon}.mim report an ERROR
```

For `vim-mim` there is no test suite: open a `.mim` file and check with `:syn list` / `:Inspect`, or eyeball `lit/docs/tutorial.mim`.

## Step 5: hand off

Three repos are involved, so this is three commits.
Do **not** commit or push - report per repo what changed and what remains, and let Roland commit.
Say explicitly if a target was left alone because the change did not reach it.
