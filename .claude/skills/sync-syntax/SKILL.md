---
name: sync-syntax
description: Propagate a change to Mim's surface syntax from the lexer/parser to the six downstream consumers - docs/langref.md, lit/docs/tutorial.mim, docs/mim.js, the vim-mim plugin, the tree-sitter-mim grammar, and the tree-sitter-mim-vscode extension. Use after touching include/mim/ast/{tok,lexer,parser}.h or src/mim/ast/{lexer,parser}.cpp or src/mim/ast/family.h, and whenever asked to check or re-sync those targets.
---

# sync-syntax

The lexer and the parser define Mim's surface syntax; six other places merely describe it and silently rot when they are not updated in the same breath.

## Ground truth

| File | What it settles |
| ---- | --------------- |
| `include/mim/ast/tok.h` | `MIM_KEY` (keywords), `MIM_SUBST` (`lm`/`bot`/`top`), `MIM_TOK` (every punctuation spelling), `MIM_INFIX_SUGAR`/`MIM_INFIX_CORE`, `MIM_PREC` (the precedence ladder and its associativity) |
| `src/mim/ast/lexer.cpp` | which byte sequence yields which tag (`lex`), the literal grammar (`lex_lit`, `lex_digits`, `lex_exp`), escapes (`lex_char`, `unquote`), comments and `///` Markdown |
| `src/mim/ast/parser.cpp` | the productions and the `Prec` bound each `parse_expr`/`parse_ptrn` passes |
| `src/mim/ast/family.h` | which tags start which production (`C_EXPR`, `C_DECL`, `C_LAM`, …) |

Start from `git diff` over those files - the diff, not the current state, is what tells you which of the six targets are in play.

## Step 1: run the checker

```sh
.claude/skills/sync-syntax/check.sh
```

It diffs the keyword and terminal tables of `tok.h` against the four targets that carry a flat word list and reports what is `missing` (in `tok.h`, not in the target) or `stale` (the other way round).
It has nothing to say about `lit/docs/tutorial.mim`, and it only sees flat word lists - a new *production*, a changed *precedence*, or a new *literal form* passes it silently too.
All of that needs the manual pass below.
It also flags capture names that `language-configs.json` of the vscode extension maps but `tree-sitter-mim/queries/highlights.scm` no longer emits.
`VIM_MIM`, `TS_MIM`, and `VSC_MIM` override the sibling checkouts (`../vim-mim`, `../tree-sitter-mim`, `../tree-sitter-mim-vscode`).

## Step 2: what a change touches

| Change | langref | tutorial | mim.js | vim-mim | tree-sitter-mim | vscode |
| ------ | :-----: | :------: | :----: | :-----: | :-------------: | :----: |
| keyword added/removed/renamed | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| new punctuation token or Unicode spelling | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| literal syntax | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| new/changed production | ✓ | ✓ | - | - | ✓ | ✓ |
| comment syntax | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| precedence or associativity | ✓ | - | - | - | ✓ | ✓ |
| error message or diagnostic only | - | - | - | - | - | - |

The vscode column only means *bump the submodule*: the extension ships `tree-sitter-mim` verbatim, so anything that reaches the grammar reaches it.
Its own files change only for brackets, comment markers, and renamed captures.

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
It is a lit test, not prose - each claim is a static `refly` assertion, so the file compiling at all is what tests it - and `mim --output-md` renders it into the docs as the Tutorial page, which is also the playground's landing example, so it is the first Mim anyone reads.

- A `///` line becomes Markdown prose and interrupts the surrounding `mim` code fence; a `//` comment stays inside it.
  So a section heading (`/// ## …`) and the sentence introducing a construct are `///`, while a remark on a single line of code is a trailing `//`.
- Show the new construct where it belongs, with a one-line `//` comment saying what it demonstrates.
  Match the surrounding density: one or two lines per construct, not a paragraph.
- Assert wherever there is something to assert.
  `refly.struc.e (a, b)` states that two *terms* are the same node; where two *types* coincide, a `lam` whose parameter type and return type differ is the assertion - as in `sigma_is_array`, `erased`, and `fuse`.
  A construct with nothing to compare against just has to typecheck.
- Both RUN lines matter: the second is `%mim -p ll %s`, so new code must also survive the ll backend.
  They live in an HTML comment at the top, so the docs page doesn't show them.
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

### `../tree-sitter-mim-vscode`

A fork of `AlecGhost/tree-sitter-vscode` that bundles two grammars as submodules and builds them to WASM with `generate_wasm.sh`; `src/extension.ts` is upstream's generic tree-sitter host.
A sync is four jobs, in this order:

1. **Own repo.** `git pull --ff-only` from `origin` (`mimir/tree-sitter-mim-vscode`) first, so nothing below lands on a stale base.
2. **Upstream.** Add the remote once (`git remote add upstream https://github.com/AlecGhost/tree-sitter-vscode.git`), then `git fetch upstream && git merge upstream/master`.
   Conflicts concentrate in `package.json` (name, publisher, version, repository, the `tree-sitter-mim-vscode.*` configuration keys) and `README.md`: keep ours for identity, take theirs for dependencies, scripts, and `engines`.
   Upstream's `tree-sitter-vscode.*` setting and command ids must stay renamed to `tree-sitter-mim-vscode.*` in both `package.json` and `src/extension.ts`.
   Beyond the renames, `src/extension.ts` carries four Mim additions to re-apply when upstream rewrites the file: the bundled `language-configs.json` with its `${extension_dir}` placeholder and per-language user overrides, last-match-wins among equal-range tokens, sorting tokens before `splitToken`, and the `#lua-match?` translation.
   `package.json` also keeps our `languages` contribution and runs `./generate_wasm.sh` in its `package` script.
   A new upstream config key (e.g. a query file like `folds`) is only live once `language-configs.json` passes it for `mim`, and the `.scm` file is copied by `generate_wasm.sh`.
3. **`tree-sitter-mim` submodule.** It must point at `mimir/tree-sitter-mim`, not the old `fodinabor/tree-sitter-mim` fork (whose history is unrelated): `git submodule set-url tree-sitter-mim https://github.com/mimir/tree-sitter-mim`, `git submodule sync`, then check out the commit the sibling `../tree-sitter-mim` was just synced to.
   Then reconcile what the extension layers over the grammar:
   - `language-configs.json`: every `semanticTokenTypeMappings` key of the `mim` entry must be a capture `queries/highlights.scm` emits - `check.sh` lists the ones that are not; an unmapped capture falls back to VS Code's defaults, a stale key is dead.
   - `mim-language-configuration.json`: `brackets`/`autoClosingPairs`/`surroundingPairs` mirror the `D_*` delimiters of `tok.h` (`<`, `>`, `<<`, `>>` are operators, not brackets); `comments` mirrors the lexer's comment forms.
4. **`tree-sitter-markdown` submodule.** Move it to the newest release tag of `tree-sitter-grammars/tree-sitter-markdown` (`git -C tree-sitter-markdown fetch --tags`, check out the highest `v*`); it only renders `///` doc comments via `queries/mim/injections.scm`, so rename checks there are all it needs.
   The `markdown`/`markdown-inline` mappings in `language-configs.json` follow its `queries/highlights.scm` the same way.

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

```sh
cd ../tree-sitter-mim-vscode
npm install && npm run build       # tsc + webpack + prettier + eslint; `prebuild` stages web-tree-sitter.wasm into dist/
npx vsce package --no-dependencies # prepublish reruns the build and generate_wasm.sh (both grammars at ABI 15)
git -C tree-sitter-mim checkout -- .   # generate_wasm.sh leaves the submodule dirty
```

`src/extension.ts` rewrites Neovim's `#lua-match?`/`#not-lua-match?` into `#match?` before compiling `highlights.scm`; web-tree-sitter silently ignores unknown predicates, which would turn every identifier into `@type` and `@constant`.
A new Lua character class in a query needs an entry in `LUA_CLASSES` there.
Headless check without VS Code: load `node_modules/web-tree-sitter/web-tree-sitter.cjs` in node, parse the `.mim` files, run the translated query, and look at which capture wins per node.

Install the `.vsix` (`code --install-extension …`) and open `lit/docs/tutorial.mim`: keywords, literals, parameters, and the Markdown inside `///` must all be coloured.

For `vim-mim` there is no test suite: open a `.mim` file and check with `:syn list` / `:Inspect`, or eyeball `lit/docs/tutorial.mim`.

## Step 5: hand off

Four repos are involved, so this is up to four commits; in the vscode repo, keep the upstream merge a commit of its own, separate from the submodule bumps.
Do **not** commit or push - report per repo what changed and what remains, and let Roland commit.
Say explicitly if a target was left alone because the change did not reach it.
