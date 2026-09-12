# Mim Language Reference {#langref}

[TOC]

This page is the reference for Mim surface syntax.

## Notation

This document uses a lightweight [EBNF](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form)-style notation.
A terminal is always quoted, a nonterminal never is, and the meta-symbols are:

- `x ::= y` defines the nonterminal `x` as `y`.
- `x | y` is either `x` or `y`.
- `(x y)` groups.
- `x*`, `x+`, and `x?` are zero or more, one or more, and an optional `x`.
- `[a-c]` is a character range from `a` to `c`, and `[a-cx-z]` combines several; ranges only occur in the [lexical rules](@ref terminals).

For example, `x ("," x)* ","?` is a comma-separated list of one or more `x` with an optional trailing comma.

## Lexical Structure {#lex}

Mim source files are [UTF-8](https://en.wikipedia.org/wiki/UTF-8) encoded and are [lexed](https://en.wikipedia.org/wiki/Lexical_analysis) from left to right.
The lexer uses [maximal munch](https://en.wikipedia.org/wiki/Maximal_munch), so ambiguities are resolved by taking the longest matching token.
For example, `>>=` is tokenized as `>>` followed by `=`.
@note `<-` is therefore a single token: `x <- 1` is an insert, and a comparison against a negative literal has to be written `x < (-1)`.

### Terminals {#terminals}

The grammar refers to *primary terminals*.
Some tokens have a second spelling - an ASCII-only one or a Unicode variant - that denotes the very same lexical token; these are the *secondary terminals*.

#### Primary Terminals

<div class="ebnf-terminals">

```text
( ) [ ] { } ⦃ ⦄
‹ › « »
→ ← => ⊥ ⊤ * □ λ
= , ; . : @ $ # | ∪
+ - / % == != < <= > >= << >>
<eof>
```

</div>

`.` is the separator of a [path](@ref path), e.g. `affine.Idx` or `core.nat.rem`.
`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `<<`, and `>>` are [infix operators](@ref infix).

#### Secondary Terminals

| Primary | Secondary |
|---------|-----------|
| `→`     | `->`      |
| `←`     | `<-`      |
| `λ`     | `lm`      |
| `⊥`     | `bot`     |
| `⊤`     | `top`     |
| `*`     | `★`       |
| `‹`     | `⟨`       |
| `›`     | `⟩`       |
| `«`     | `⟪`       |
| `»`     | `⟫`       |

#### Keywords

<div class="ebnf-terminals">

```text
Bool Cn Fn I1 I8 I16 I32 I64 Idx Nat Rule Type Univ
and anx as axm cn con end extern ff fn fun
i1 i8 i16 i32 i64 import inj lam let match mod
norm plugin priv pub rec ret rule tt use when where with
```

</div>

The following names are predefined aliases:

```text
tt   = 1₂
ff   = 0₂
Bool = Idx i1
I1   = Idx i1
I8   = Idx i8
I16  = Idx i16
I32  = Idx i32
I64  = Idx i64

i1   = 2
i8   = 0x100
i16  = 0x1'0000
i32  = 0x1'0000'0000
i64  = 0
```

#### Pattern Terminals

The following terminals are defined by lexical patterns.

```ebnf
I      ::= id
        |  "`" op
L      ::= dec+
        |  "0" ("b" | "B") bin+
        |  "0" ("o" | "O") oct+
        |  "0" ("x" | "X") hex+
        |  dec+ eE sign? dec+
        |  dec+ "." dec* (eE sign? dec+)?
        |  dec* "." dec+ (eE sign? dec+)?
        |  "0" ("x" | "X") hex+ pP sign? dec+
        |  "0" ("x" | "X") hex+ "." hex* pP sign? dec+
        |  "0" ("x" | "X") hex* "." hex+ pP sign? dec+
X_n    ::= dec+ sub+
        |  dec+ "_" dec+
        |  dec+ ("i" | "I") dec+
C      ::= "'" (ascii_char | esc) "'"
S      ::= "\"" (ascii_string_char | esc)* "\""
```

Here `I` is an identifier, `L` is a numeric literal, `X_n` is an index literal of type `Idx n`, `C` is a character literal, and `S` is a string literal.
The third form of `X_n` spells a bit width instead of `n` itself, so `23I32` is `23:I32`.
A literal never carries a sign; `-23` is the [signed literal](@ref lit) expression instead.
`` ` `` escapes an [infix operator](@ref infix) into an ordinary identifier, e.g. `` `+ ``.

The shorthand symbols used above are:

```ebnf
bin    ::= [0-1]
oct    ::= [0-7]
dec    ::= [0-9]
sub    ::= [₀-₉]
hex    ::= [0-9a-fA-F]
eE     ::= "e" | "E"
pP     ::= "p" | "P"
sign   ::= "+" | "-"
id     ::= ("_" | [a-zA-Z]) ("_" | [0-9a-zA-Z])*
op     ::= "+" | "-" | "*" | "/" | "%"
esc    ::= "\'" | "\"" | "\0" | "\a" | "\\" | "\b"
        |  "\f" | "\n" | "\r" | "\t" | "\v"
```

Character and string literals only admit ASCII payload characters plus the escapes listed above.

### Comments

Mim supports `/* ... */` multi-line comments, `// ...` single-line comments, and `/// ...` comments that are forwarded to generated [Markdown](https://www.doxygen.nl/manual/markdown.html) output.
`/* ... */` comments are not nested.
For `///` comments, a line of the form `/// text` contributes `text` directly to the Markdown output.
Other `///` forms are emitted verbatim inside a [fenced code block](https://www.doxygen.nl/manual/markdown.html#md_fenced).

## Grammar {#grammar}

Mim is defined by a [context-free grammar](https://en.wikipedia.org/wiki/Context-free_grammar).
Its terminals are the lexical elements defined above.
The start symbol is `f` for *file*.

The main nonterminals used below are:

| Symbol | Meaning            |
|--------|--------------------|
| `f`    | file               |
| `d`    | declaration        |
| `p`    | `()`-style pattern |
| `b`    | `[]`-style pattern |
| `e`    | expression         |

### Files and Imports {#module}

```ebnf
f ::= d*
```

A file is a sequence of declarations.
Each file forms a [module](@ref path) of its own that an import binds under a name.

- `import foo;` resolves the module name `foo` through the search path.
- `import "some/dir/foo.mim";` resolves the path relative to the importing file first, then through the search path.
- If the resolved file name has no extension, `.mim` is appended.
- `plugin foo;` first loads the plugin `foo` and then imports the module with the same name.
  Only `plugin` loads a shared object; `import` never does.
- The bound name defaults to the file name without its extension and must be an identifier; `as` overrides it.
- `import foo as *;` splices `foo`'s public members into the current scope like a following `use foo;` would - except that `foo` itself is never bound; the same goes for `plugin foo as *;`.
  Since no name is needed, this form also accepts a file name that isn't an identifier.
- A file is parsed, bound, and emitted exactly once, no matter how many modules import it.
  Importing a file that is still being parsed is an error.
- An import is an ordinary declaration, so it may sit wherever declarations may - inside a `mod`, a `where` block, or a function body - and binds its name in exactly that scope.

### Paths and Modules {#path}

```ebnf
path ::= I ("." I)*
```

A path resolves its first component lexically and then walks into that module.
A module is either an imported file or a `mod` declaration.

- A component after a `.` may be spelled like a keyword, as may a tag in an `axm` tag list - both positions are unambiguous.
- `.` never reads a field out of a value; use `#` for that.
- An `anx` declaration is an ordinary member of its enclosing module and is found by the same path resolution as any other member; see [Annex](@ref annex) for what additionally makes it an annex.

### Declarations {#decl}

Mim supports the following declaration families.
Most of them may be prefixed with any combination of three independent modifiers:
a visibility (`priv` or `pub`), `extern`, and `anx`.
Visibility is a Mim-only, purely lexical fact - it has no effect on backend linkage or compiler registration.
`extern` and `anx` are each independent of visibility and of each other;
either one nudges the default visibility to `pub` (instead of the usual `priv` default) unless `priv`/`pub` is given explicitly, so e.g. `priv anx` and `priv extern` are legal and meaningful, while `extern anx` on the same declaration is a static error.

```ebnf
d      ::= "import" (I | S) ("as" (I | "*"))? ";"
        |  "plugin" I ("as" (I | "*"))? ";"
        |  vis? "mod" I "{" d* "}"
        |  "use" path ("as" (I | "*"))? ";"
        |  vis? "anx"? "let" p "=" e
        |  vis? "anx" I "=" path
        |  vis? ("extern" | "anx")? lam I dom+ (":" e)? "=" e and*
        |  vis? "extern" lam I dom+ (":" e)? ";"
        |  vis? "anx"? "rec" I (":" e)? "=" e and*
        |  vis? "axm" axm
        |  ("rule" | "norm") I p ":" e ("when" e)? "=>" e

and    ::= "and" I (":" e)? "=" e
        |  "and" lam I dom+ (":" e)? "=" e
vis    ::= "priv" | "pub"
lam    ::= "lam" | "con" | "fun"
dom    ::= p ("@" e)?
axm    ::= I ":" e tail
        |  (I ".")? "(" (tag ("," tag)* ","?)? ")" ":" e tail
tag    ::= I ("=" I)*
tail   ::= ("," I)? ("," L ("," L)?)?
```

- `priv` restricts a declaration to its lexical scope: a path may not cross into it from outside its enclosing `mod`; it is the default visibility unless `extern` or `anx` nudges it to `pub`.
- `pub` lifts that restriction, so a path from outside the enclosing `mod` may reach the declaration.
- `anx` marks a declaration as an [annex](@ref annex). It doesn't apply to `mod`, since a module is pure AST grouping, not a single value. `axm` is implicitly `anx` and may not combine with `extern`.
- `import` and `plugin` bind a file as a module, or splice its public members into the current scope; see [Files and Imports](@ref module).
- `mod` groups declarations under a name; its body also sees the enclosing scope. Neither `extern` nor `anx` apply to it.
- `use path as I` introduces `I` as another name for the module `path` denotes; `use path as *` splices that module's public members into the current scope instead, and a plain `use path` is sugar for the latter.
- `let` introduces a binding pattern.
- `anx I = path` declares `I` as an alias for the annex denoted by `path`.
- `lam`, `con`, and `fun` declare lambdas, continuations, and returning continuations.
- `extern` makes a `lam`/`con`/`fun` declaration a root of the `World` that stays reachable through `Cleanup` and is visible to backends; with the body omitted (just `;`), its implementation lives in a native translation unit instead. Currently, `extern` is only meaningful on a `lam`/`con`/`fun` declaration.
- The `@` of a `dom` introduces its partial-evaluation filter.
- `rec` starts a recursive declaration group, and `and` extends the same group.
- After `and`, the next declaration may be another `rec`-style binding or an explicit `lam`, `con`, or `fun` declaration; an `and`-continuation doesn't accept its own modifiers.
- `axm` declares an axiom.
  A `tag` list declares several axioms of the same type at once, and each `"=" I` adds another name for that tag.
  Prefixed with a name as in `axm nat.(add, sub): ...`, the tags become members of a module of that name.
- An `axm`'s `tail` is the normalizer, the curry counter, and the trip count, in that order; a trip count requires a curry counter.
- `rule` and `norm` declare rewrite rules.
- `norm` is the normalizing variant of `rule`.

### Patterns {#ptrn}

Patterns decompose values and describe binders.

```ebnf
p   ::= I (":" e)?
     |  "(" (pg ("," pg)* ","?)? ")"
     |  b
     |  p "as" I

pg  ::= p
     |  g

b   ::= I (":" e)?
     |  "[" (bg ("," bg)* ","?)? "]"
     |  b "as" I
     |  e

bg  ::= b
     |  g

g   ::= I+ ":" e
```

There are two pattern families.

- `p` is the ordinary parenthesized binder syntax.
- `b` is the bracketed syntax used for sigma binders and Pi domains.
- Roughly speaking, `(a, b, c)` binds tuple components with inferred types, while `[a, b, c]` binds components whose types are described by the bracket entries.
- When all component types are written explicitly, `(a: A, b: B)` and `[a: A, b: B]` coincide.
- Tuple patterns support grouped bindings such as `(a b c: Nat, d e: Bool)` and `[a b c: Nat, d e: Bool]`.
- Both forms distribute the annotated type over the listed names.
- Patterns may be wrapped in an alias pattern.

<div class="mim-code">

```mim
let (a, b, c) as abc = (1, 2, 3);
```

</div>

This binds `a`, `b`, and `c` to the tuple elements and `abc` to the whole tuple.

- Bracket-style patterns may also contain general expressions.
- This is what makes forms such as `[T: *] → T` and `Cn [mem: mem.M 0, I32]` legal.
- `let` and `ret` allow rebinding of an existing name.

This is especially useful for state-threading style code:

<div class="mim-code">

```mim
let (mem, ptr) = mem.alloc (I32, 0) mem;
let mem        = mem.store (mem, ptr, 23:I32);
let (mem, val) = mem.load (mem, ptr);
```

</div>

### Expressions {#expr}

#### Kinds and Builtin Types

```ebnf
e   ::= "Univ"
     |  "Type" e
     |  "*"
     |  "□"
     |  "Nat"
     |  "Idx"
     |  "Bool"
     |  "Rule" e
```

- `Univ` is the universe of type levels.
- `Type e` is the type at level `e`.
- `*` abbreviates `Type (0:Univ)`.
- `□` abbreviates `Type (1:Univ)`.
- `Nat` is the natural number type.
- `Idx` is the builtin of type `Nat → *`.
- `Bool` abbreviates `Idx i1`.
- `Rule e` is the type of rewrite rules over the meta type `e`.

#### Literals and Basic Forms {#lit}

```ebnf
e   ::= sign? L (":" e)?
     |  sign? X_n
     |  "ff"
     |  "tt"
     |  C
     |  S
     |  "⊥" (":" e)?
     |  "⊤" (":" e)?
     |  path
     |  d+ e
     |  "⦃" e "⦄"
```

- A numeric, character, string, `⊥`, or `⊤` literal may carry an explicit type ascription.
- A `+`/`-` sign is part of the literal expression, not of the literal token, and only a numeric literal accepts one.
  Because `+` and `-` are [infix operators](@ref infix) everywhere else, `f -23` subtracts; pass a negative argument as `f (-23)`.
- Without an explicit type, numeric literals default to `Nat`.
- Without an explicit type, `⊥` and `⊤` default to `*`.
- `d+ e` is a declaration expression: one or more declarations followed by a final expression `e`, which is the result.
  It is not delimited by any brackets; it simply starts with a declaration keyword such as `let`.
- `⦃ e ⦄` is a singleton type.

#### Functions and Continuations

```ebnf
e   ::= e "→" e
     |  b "→" e
     |  "Cn" b
     |  "Fn" b "→" e
     |  "λ" p+ (":" e)? "=" e
     |  "cn" p+ (":" e)? "=" e
     |  "fn" p+ (":" e)? "=" e
     |  e e
     |  e "@" e
     |  "ret" p "=" e "$" e ";" e
```

- `e → e` is the ordinary arrow type.
- `b → e`, `Cn b`, and `Fn b → e` are dependent function forms whose domain is described by a bracket-style pattern.
- `λ`, `cn`, and `fn` are the expression forms corresponding to `lam`, `con`, and `fun`.
- Application is written by juxtaposition.
- `e @ e` passes an explicit implicit argument.
- `ret p = callee $ arg; body` binds the result of a continuation-style call and continues with `body`.

#### Products

```ebnf
e   ::= "[" (bg ("," bg)* ","?)? "]"
     |  "(" (e ("," e)* ","?)? ")"
     |  "«" arity ("," arity)* ";" e "»"
     |  "‹" arity ("," arity)* ";" e "›"
     |  e "#" e
     |  e "#" I
     |  e "#" e "←" e
     |  e "←" e

arity ::= e
       |  I ":" e
```

- `[ ... ]` is a sigma expression unless it is immediately followed by `as` or `→`, in which case it is parsed as the domain of a dependent function type.
- `( ... )` is a tuple expression.
- `« ... ; ... »` builds an array.
- `‹ ... ; ... ›` builds a pack.
- An array or pack may have several comma-separated dimensions, as in `«i: m, j: n; body»`, and each dimension may optionally be named.
- `e # i` or `e # e` extracts a component.
- `tuple#index ← value` yields a **new** aggregate with `index` replaced by `value`; it does not mutate `tuple`.
- `e ← value` without a `#` leaves the index implicit: `e` is its own sole component, so this is `e#0₁ ← value` and hence `value`.
  An `e` of arity other than 1 is an error, just as `e#0₁` would be.
- `←` binds weaker than application, so `f t#i ← v` is `(f t#i) ← v` - write `f (t#i ← v)`.

#### Unions

```ebnf
e   ::= e "∪" e
     |  e "inj" e
     |  "match" e "with" ("|"? p "=>" e)+
```

- `e ∪ t` forms a union type.
- `e inj t` injects a value into a union type.
- `match e with | p => e | ...` eliminates a union value.

#### Infix Operators {#infix}

```ebnf
e   ::= e "==" e
     |  e "!=" e
     |  e "<" e
     |  e "<=" e
     |  e ">" e
     |  e ">=" e
     |  e "<<" e
     |  e ">>" e
     |  e "+" e
     |  e "-" e
     |  e "*" e
     |  e "/" e
     |  e "%" e
```

- `a op b` is sugar for `` `op (a, b) ``, so `a + b` is `` `+ (a, b) ``.
- Mim doesn't give the operators a meaning of their own; whatever `` `op `` is bound to is what they mean:
  <div class="mim-code">

  ```mim
  let `+ = core.nat.add;
  let x = 2 + 3;
  ```

  </div>
- `*` doubles as the abbreviation of `Type (0:Univ)`, which is why `f *` is a multiplication and not an application; write `f (*)` for the latter.

#### Local Declaration Blocks

```ebnf
e   ::= e "where" d* "end"
```

`where` attaches a local declaration block to an already parsed expression.
`where` blocks bind more weakly than the other infix expression forms.

### Precedence {#prec}

Parser and dumper share one ladder of precedence levels, listed here from strongest to weakest binding.
*Assoc* is left-, right-, or non-associative; chaining a non-associative operator, as in `a == b == c`, is an error - parenthesize one side.
`Pi`, `Bot`, and `Err` are *pseudo levels*: they name no syntax at all and only ever bound how far a nested expression may extend.

|  # | Level     | Assoc | Operators                            | Notes                                                                    |
|---:|-----------|:-----:|--------------------------------------|--------------------------------------------------------------------------|
|  1 | `Lit`     |   -   | `L : e`                              | The tightest level. Not an infix operator - the literal parser reads the ascription itself and bounds `e` here. |
|  2 | `Extract` | left  | `e # e`, `e # I`                     |                                                                          |
|  3 | `App`     | left  | `e e`, `e @ e`                       | Application binds tighter than every operator. Also bounds the `e` in `Type e` and `Rule e`. |
|  4 | `Shift`   | left  | `e << e`, `e >> e`                   | Tighter than `*`, as in Lean and OCaml - not the C position.             |
|  5 | `Mul`     | left  | `e * e`, `e / e`, `e % e`            |                                                                          |
|  6 | `Add`     | left  | `e + e`, `e - e`                     |                                                                          |
|  7 | `Rel`     | none  | `e < e`, `e <= e`, `e > e`, `e >= e` |                                                                          |
|  8 | `Eq`      | none  | `e == e`, `e != e`                   |                                                                          |
|  9 | `Pi`      |   -   | *pseudo*                             | Bounds the domain of a `λ`/`Fn`/`b → e` binder so it stops before the `→`. A `Cn`-style binder has no `→` and uses `Bot` instead. |
| 10 | `Arrow`   | right | `e → e`                              | Also bounds the codomain after a `→`.                                    |
| 11 | `Union`   | left  | `e ∪ e`                              |                                                                          |
| 12 | `Inj`     | right | `e inj e`                            | Weaker than `∪`, so `x inj A ∪ B` is `x inj (A ∪ B)`.                    |
| 13 | `Ins`     | right | `e#e ← e`, `e ← e`                   | Also bounds a declaration's `: codom` slot, which ends at `=` and so takes everything short of a `where`. |
| 14 | `Where`   | left  | `e where d* end`                     | The loosest surface operator.                                            |
| 15 | `Bot`     |   -   | *pseudo*                             | A complete expression; the default bound, and the only one a trailing `where` fits into. |
| 16 | `Err`     |   -   | *pseudo*                             | Below everything; the parser's "no operator seen yet" sentinel.          |

## Summary: Functions and Types

Mim uses different surface syntax for declarations, expressions, and types:

| Declaration | Expression   | Type |
|-------------|--------------|------|
| `lam`       | `λ` / `lm`   | `→`  |
| `con`       | `cn`         | `Cn` |
| `fun`       | `fn`         | `Fn` |

### Declarations

The following declarations are equivalent:

<div class="mim-code">

```mim
lam f(T: *)((x y: T), return: T → ⊥)@ff: ⊥ = return x;
con f(T: *)((x y: T), return: Cn T)        = return x;
fun f(T: *) (x y: T): T                    = return x;
```

</div>

Partial-evaluation filters default to `tt`, except for `con`, `cn`, `fun`, and `fn`.

### Expressions

The following expressions are equivalent.
Because they are bound by `let`, they behave like the declarations above:

<div class="mim-code">

```mim
let f =  λ (T: *) ((x y: T), return: T → ⊥)@ff: ⊥ = return x;
let f = lm (T: *) ((x y: T), return: T → ⊥)   : ⊥ = return x;
let f = cn (T: *) ((x y: T), return: Cn T)        = return x;
let f = fn (T: *)  (x y: T): T                    = return x;
```

</div>

### Applications

The following applications of `f` are equivalent:

<div class="mim-code">

```mim
f Nat ((23, 42), cn res: Nat = use(res))
ret res = f Nat $ (23, 42); use(res)
```

</div>

### Function Types

The following types are equivalent and describe the type of `f` above:

<div class="mim-code">

```mim
[T: *] →    [[T, T], T → ⊥] → ⊥
[T: *] → Cn [[T, T], Cn T]
[T: *] → Fn  [T, T] → T
```

</div>

## Scoping

Mim uses [lexical scoping](<https://en.wikipedia.org/wiki/Scope_(computer_science)#Lexical_scope>).
Unless noted otherwise, all names live in the same scope.
A file is bound in isolation: it never sees the scope of whoever imports it.

### Underscore

The symbol `_` is special: it never binds an entity.
As a consequence, `_` may appear repeatedly in the same scope without conflict, but any use of `_` as a reference is a scoping error.

### Annex {#annex}

An `anx` declaration is an ordinary member of its enclosing module, found by the same path resolution as any other member.
Its plugin-qualified name (`plugin.tag[.sub]`, derived from `mod` nesting) is additionally registered in a global by-name table (@ref mim::Annex) that tools such as `compile.named` and plugin bootstrap use to look an annex up directly by that name, independent of the surrounding modules.

### Field Names of Sigmas

Named elements of mutable sigma types are available for extracts and inserts.
@note These names take precedence over ordinary lexical names.
In the example below, `i` refers to the field name of `X`, not the `let`-bound variable:

<div class="mim-code">

```mim
let i = 1_2;
[i: Nat, j: Nat]::X → f X#i;
```

</div>

Use parentheses to force the variable interpretation:

<div class="mim-code">

```mim
let i = 1_2;
[i: Nat, j: Nat]::X → f X#(i);
```

</div>

<div class="section_buttons">

| Previous |     Next |
|:---------|---------:|
| [Command-Line Reference](@ref cli) | [Contributing \& Debugging](@ref coding) |

</div>
