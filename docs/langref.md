# Mim Language Reference {#langref}

[TOC]

This page is the reference for Mim surface syntax.

@note The [Tutorial](@ref tutorial) shows every construct defined here in one annotated file.
That page is `lit/docs/tutorial.mim` rendered by `mim --output-md`, and the file asserts what it shows with [refly](@ref refly), so the test suite keeps the tour honest.
It is also the [playground](@ref playground)'s landing example - open it there to run and edit the tour in the browser.

## Notation

This document uses a lightweight [EBNF](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form)-style notation.
A terminal is shaded, a nonterminal is not, and the [lexical terminals](@ref terminals) `I`, `L`, `X_n`, `C`, and `S` have a colour of their own.
Clicking a nonterminal or a lexical terminal traces its occurrences across the page.
The meta-symbols are:

```ebnf
x ::= y  // defines the nonterminal x as y
x | y    // either x or y
(x y)    // grouping
x*       // zero or more x
x+       // one or more x
x?       // an optional x
[a-c]    // character range from a to c
[a-cx-z] // combines several ranges
```

For example, `x ("," x)*` is a comma-separated list of one or more `x`.
@note Ranges only occur in the rules of the [lexical terminals](@ref terminals).
@note Every comma-separated list additionally accepts a trailing `,`, as in `(e, e, e,)`.
The rules below don't spell this out.

## Lexical Structure {#lex}

Mim source files are [UTF-8](https://en.wikipedia.org/wiki/UTF-8) encoded and are [lexed](https://en.wikipedia.org/wiki/Lexical_analysis) from left to right.
The lexer uses [maximal munch](https://en.wikipedia.org/wiki/Maximal_munch), so ambiguities are resolved by taking the longest matching token.
For example, `>>=` is tokenized as `>>` followed by `=`.
@note `<-` is therefore a single token: `x <- 1` is an insert, and a comparison against a negative literal has to be written `x < (-1)`.

### Terminals {#terminals}

The grammar refers to _primary terminals_.
Some tokens have a second spelling - an ASCII-only one or a Unicode variant - that denotes the very same lexical token; these are the _secondary terminals_.

#### Primary Terminals

<div class="terminals-code">

```text
( ) [ ] { }
‹ › « »
→ ← => ⊥ ⊤ * □ λ
= , ; . : @ $ # | ∪
+ - / % == != < <= > >= << >>
<eof>
```

</div>

- `.` is the separator of a [path](@ref path), e.g. `affine.Idx` or `core.nat.rem`.
- `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `<<`, and `>>` are [infix operators](@ref infix).
- `#` is an infix operator after an expression and a prefix one everywhere else; see [Singletons](@ref single).

#### Secondary Terminals

| Primary | Secondary |
| ------- | --------- |
| `→`     | `->`      |
| `←`     | `<-`      |
| `λ`     | `lm`      |
| `⊥`     | `bot`     |
| `⊤`     | `top`     |
| `*`     | `★`       |

#### Keywords

<div class="terminals-code">

```text
Bool Cn Fn I1 I8 I16 I32 I64 Idx Nat Rule Type Univ
and anx as axm cn con end extern ff fn fun
i1 i8 i16 i32 i64 import inj lam let match mod
norm plugin priv pub rec ret rule tt use when where with
```

</div>

The following names are predefined aliases:

| Alias  | Expansion       |
| ------ | --------------- |
| `tt`   | `1₂`            |
| `ff`   | `0₂`            |
| `Bool` | `Idx i1`        |
| `I1`   | `Idx i1`        |
| `I8`   | `Idx i8`        |
| `I16`  | `Idx i16`       |
| `I32`  | `Idx i32`       |
| `I64`  | `Idx i64`       |
| `i1`   | `2`             |
| `i8`   | `0x100`         |
| `i16`  | `0x1'0000`      |
| `i32`  | `0x1'0000'0000` |
| `i64`  | `0`             |

@note 2⁶⁴ doesn't fit into a `Nat`, so `i64` is `0`, which `Idx` reads as 2⁶⁴.

#### Lexical Terminals

The following terminals are defined by lexical rules.

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
        |  "==" | "!=" | "<" | "<=" | ">" | ">="
        |  "<<" | ">>"
esc    ::= "\'" | "\\" | "\"" | "\0" | "\a" | "\b"
        |  "\f" | "\n" | "\r" | "\t" | "\v"
```

`ascii_char` is any ASCII character except `\`, `ascii_string_char` is any ASCII character except `\` and `"`, and a non-ASCII payload character is an error.

### Comments

Supported comments:

```mim
/* multi-line comment */
// single-line comment
/// doc-comment
```

- `/* ... */` comments do not nest.
- doc-comments are forwarded to generated [Markdown](https://www.doxygen.nl/manual/markdown.html) output.
  A line of the form `/// text` contributes `text` directly to the Markdown output.
  Other `///` forms are emitted verbatim inside a [fenced code block](https://www.doxygen.nl/manual/markdown.html#md_fenced).

## Grammar {#grammar}

Mim is defined by a [context-free grammar](https://en.wikipedia.org/wiki/Context-free_grammar).
Its terminals are the lexical elements defined above.
The start symbol is `f` for _file_.

The main nonterminals used below are:

| Symbol | Meaning     |
| ------ | ----------- |
| `f`    | file        |
| `d`    | declaration |
| `p`    | pattern     |
| `t`    | telescope   |
| `e`    | expression  |

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
- `plugin "some/dir/foo";` looks for `libmim_foo` in `some/dir` below each plugin search path and takes `foo.mim` from wherever that library was found.
- The bound name defaults to the file name without its extension and must be an identifier; `as` overrides it.
- `import foo as *;` splices `foo`'s public members into the current scope like a following `use foo;` would - except that `foo` itself is never bound; the same goes for `plugin foo as *;`.
  Since no name is needed, this form also accepts a file name that isn't an identifier.
- An import is `priv` unless declared `pub`: `import foo;` binds `foo` privately, while `pub import foo;` re-exports it, and `pub import foo as *;` re-exports every spliced member.
- A file is parsed, bound, and emitted exactly once, no matter how many modules import it.
  Importing a file that is still being parsed is an error.
- An import is an ordinary declaration, so it may sit wherever declarations may - inside a `mod`, a `where` block, or a function body - and binds its name in exactly that scope.

### Paths and Modules {#path}

```ebnf
path ::= I ("." I)*
```

A path resolves its first component lexically and then walks into that module.
A module is either an imported file or a `mod` declaration.

- Every component of a path is an identifier; a keyword is not allowed, not even after a `.`.
- `.` never reads a field out of a value; use `#` for that.
- An `anx` declaration is an ordinary member of its enclosing module and is found by the same path resolution as any other member; see [Annex](@ref annex) for what additionally makes it an annex.

### Declarations {#decl}

Mim supports the following declaration families.
Most of them may be prefixed with a visibility (`priv` or `pub`), `extern`, and `anx`.
These modifiers may be written in any order; the productions below only spell out which of them a declaration accepts at all.
Visibility is a Mim-only, purely lexical fact - it has no effect on backend linkage or compiler registration.
`extern` and `anx` are each independent of visibility, but not of each other: `extern anx` on the same declaration is a static error.
Either one nudges the default visibility to `pub` (instead of the usual `priv` default) unless `priv`/`pub` is given explicitly, so e.g. `priv anx` and `priv extern` are legal and meaningful.

```ebnf
d      ::= vis? import (I | S) ("as" (I | "*"))? ";"
        |  vis? "use"  path    ("as" (I | "*"))? ";"
        |  vis? "mod" I "{" d* "}"
        |  vis? "anx"? "let" p "=" e
        |  vis? "anx" I "=" path
        |  vis? ("extern" | "anx")? lam I dom+ (":" e)? "=" e and*
        |  vis?  "extern"          lam I fwd+ (":" e)? ";"
        |  vis? "anx"? "rec" I "=" e and*
        |  vis? "axm" axm
        |  ("rule" | "norm") I p ":" e ("when" e)? "=>" e

import ::= "import" | "plugin"
and    ::= "and" I "=" e
        |  "and" lam I dom+ (":" e)? "=" e
vis    ::= "priv" | "pub"
lam    ::= "lam" | "con" | "fun"
dom    ::= p ("@" e)?
fwd    ::= (p | t) ("@" e)?
axm    ::= I ":" e tail
        |  (I ".")? "(" (tag ("," tag)*)? ")" ":" e tail
tag    ::= I ("=" I)*
tail   ::= ("," I)? ("," L ("," L)?)?
```

@note A declaration may be followed by a `;`, as all examples on this page do; stray semicolons between declarations are skipped.

- `import` and `plugin` bind a file as a module, or splice its public members into the current scope; see [Files and Imports](@ref module).
- `use path as I` introduces `I` as another name for the module `path` denotes; `use path as *` splices that module's public members into the current scope instead, and a plain `use path` is sugar for the latter.
- `mod` groups declarations under a name; its body also sees the enclosing scope.
  Neither `extern` nor `anx` apply to it.
- `let` introduces a binding pattern.
- `anx I = path` declares `I` as an alias for the annex denoted by `path`.
- `lam`, `con`, and `fun` declare lambdas, continuations, and returning continuations.
  The declared name is already in scope inside its own body, so such a declaration is recursive.
  `and` extends the group to mutual recursion; a forward reference without `and` does not resolve.
  The corresponding [expression forms](@ref expr) are anonymous and cannot refer to themselves.
- The `@` of a `dom` introduces its partial-evaluation filter.
- `rec` starts a recursive declaration group, and `and` extends the same group.
  Its body must be a sigma, a [variant](@ref variant), or a function type, as those are built as a mutable and filled in afterwards, so that `I` is already in scope inside it; its universe level is inferred from the body.
  A recursive _function_ is declared with `lam`/`con`/`fun` instead.
- After `and`, the next declaration may be another `rec`-style binding or an explicit `lam`, `con`, or `fun` declaration; an `and`-continuation doesn't accept its own modifiers.
- `axm` declares an axiom.
  A `tag` list declares several axioms of the same type at once, and each `= I` adds another name for that tag.
  Prefixed with a name as in `axm nat.(add, sub): ...`, the tags become members of a module of that name.
- An `axm`'s `tail` is the normalizer, the curry counter, and the trip count, in that order; a trip count requires a curry counter.
- `rule` and `norm` declare rewrite rules.
- `norm` is the normalizing variant of `rule`.

#### Modifiers

- `anx` marks a declaration as an [annex](@ref annex).
  It doesn't apply to `mod`, since a module is pure AST grouping, not a single value.
  `axm` is implicitly `anx` and may not combine with `extern`.
- `extern` is currently only meaningful on a `lam`/`con`/`fun` declaration and makes it
  - **with a body** a root of the `World` that stays reachable through `Cleanup`.
    Backends emit it under its source name with external linkage, so other translation units can call it, whereas a non-`extern` function gets a mangled name and internal linkage.
  - **without a body** (just `;`) a declaration for another translation unit.
    Contrary to its `extern` annotation, such a stub is not [external](@ref mim::Def::is_external) in the IR sense and, if unused, is removed like any other unreachable `Def`.

##### Visibility

- `priv` restricts a declaration to its lexical scope: a path may not cross into it from outside its enclosing `mod`; it is the default visibility unless `extern` or `anx` nudges it to `pub`.
- `pub` lifts that restriction, so a path from outside the enclosing `mod` may reach the declaration.
- Visibility belongs to the _binding_, not to the declaration it names: an `import`/`plugin`/`use` that splices `as *` re-binds someone else's declarations under its own visibility, so only a `pub` splice re-exports them, no matter how public they were in their own module.

### Patterns and Telescopes {#ptrn}

A pattern decomposes a value; a telescope describes a type.

```ebnf
p     ::= I (":" e)?
       |  "(" plist? ")"
       |  p "as" I

t     ::= I (":" e)?
       |  "[" tlist? "]"
       |  t "as" I
       |  e

plist ::= (p | g) ("," (p | g))*
tlist ::= (t | g) ("," (t | g))*
g     ::= I+ ":" e
```

These are two different things, not two spellings of one thing.

- A **pattern** `p` destructs a value into names that a body uses, so it only appears where a body exists.
- A **telescope** `t` describes a type and names a component only so that later components or the codomain may depend on it.
- The two grammars are identical except that a telescope additionally admits a bare `e`.
  Hence the whole difference: **an unnamed element is a binder in `(...)` and a type in `[...]`**.
  `(a, b, c)` binds three components with inferred types; `[A, B, C]` describes three unnamed components of those types.
- A **group** `g` distributes one annotated type over several names, as in `(a b c: Nat, d e: Bool)` or `[a b c: Nat, d e: Bool]`.
  It is only ever an element of a `plist`/`tlist`, never a `p`/`t` of its own.
- Only a telescope may contain general expressions, which is what makes `[T: *] → T` and `Cn [mem: mem.M 0, I32]` legal.
- `[...]` never binds for a body: a declaration or `λ`/`cn`/`fn` **with** a body must spell its domain as a `(...)` pattern, and writes an unnamed component as `_: T`.
  A bodyless `extern` declaration accepts either, since nothing binds there anyway.

A telescope name is visible only to what stands to its right - later components, and the codomain after a `→`.
`Cn X` abbreviates `X → ⊥` and so has no codomain at all, which makes every name in `Cn [x y: I32]` erased: it is the very same type as `Cn [I32, I32]`.

A telescope is not a form of its own but the finite end of one construct.
`[...]` is a [sigma](@ref prod), and a sigma whose components are all the same _is_ an [array](@ref prod): `[Nat, Nat, Nat]` and `«3; Nat»` denote one and the same type.
The same compression applies once more to a nest of arrays: `«2; «3; T»»` and `«2, 3; T»` denote one and the same type, and an array carries all of its axes as one _shape_.
`#` follows suit - `t#i#j` and `t#(i, j)` are the same Extract - so an index has exactly as many components as the shape it indexes into.
The sole exception is a [mutable sigma](@ref mutsigma) of one component.
A sigma names a component so that later components may depend on it; an array names its index so that the element type may depend on that.
`«i: n; T i»` is therefore the very same dependency, taken over an arity that need not be a literal.

The term level mirrors the type level: `(...)` is a tuple, `‹n; e›` a pack, and a tuple of `n` equal elements _is_ that pack - `(0, 0, 0)` and `‹3; 0›` are the same value, while `(23, 42, 66)` stays a tuple.
`#` extracts from all four alike.
Hence `[n: Nat, «n; T»]` describes a function whose number of arguments is a runtime value - a telescope of its own could never spell that, since it fixes its length syntactically.

An alias pattern wraps another pattern and additionally binds the whole value:

```mim
let (a, b, c) as abc = (1, 2, 3);
```

This binds `a`, `b`, and `c` to the tuple elements and `abc` to the whole tuple.

`let` and `ret` allow rebinding of an existing name, which is especially useful for state-threading style code:

```mim
let (m, ptr) = mem.alloc (I32, 0) m;
let m        = mem.store (m, ptr, 23:I32);
let (m, val) = mem.load (m, ptr);
```

### Expressions {#expr}

#### Kinds and Builtin Types

```ebnf
e     ::= "Univ"
       |  "Type" e
       |  "*"
       |  "□"
       |  "Nat"
       |  "Idx"
       |  "Rule" e
       |  alias

alias ::= "Bool"
       |  "I1" | "I8" | "I16" | "I32" | "I64"
       |  "i1" | "i8" | "i16" | "i32" | "i64"
```

- `Univ` is the universe of type levels.
- `Type e` is the type at level `e`.
- `*` abbreviates `Type (0:Univ)`.
- `□` abbreviates `Type (1:Univ)`.
- `Nat` is the natural number type.
- `Idx` is the builtin of type `Nat → *`.
- `Rule e` is the type of rewrite rules over the meta type `e`.
- `alias` is one of the [predefined aliases](@ref terminals), so `Bool` abbreviates `Idx i1` and `I32` abbreviates `Idx i32`.

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
```

- A numeric, character, string, `⊥`, or `⊤` literal may carry an explicit type ascription.
- A `+`/`-` sign is part of the literal expression, not of the literal token, and only a numeric literal accepts one.
  Because `+` and `-` are [infix operators](@ref infix) everywhere else, `f -23` subtracts; pass a negative argument as `f (-23)`.
- Without an explicit type, numeric literals default to `Nat`.
- Without an explicit type, `⊥` and `⊤` default to `*`.
- `d+ e` is a declaration expression: one or more declarations followed by a final expression `e`, which is the result.
  It is not delimited by any brackets; it simply starts with a declaration keyword such as `let`.

#### Functions and Continuations

```ebnf
e   ::= e "→" e
     |  t "→" e
     |  "Cn" t
     |  "Fn" t "→" e
     |  "λ" p+ (":" e)? "=" e
     |  "cn" p+ (":" e)? "=" e
     |  "fn" p+ (":" e)? "=" e
     |  e e
     |  e "@" e
     |  "ret" p "=" e "$" e ";" e
```

- `e → e` is the ordinary arrow type.
- `t → e`, `Cn t`, and `Fn t → e` are dependent function forms whose domain is described by a telescope.
- `λ`, `cn`, and `fn` are the expression forms corresponding to `lam`, `con`, and `fun`.
  They are anonymous, so they cannot call themselves; use a `lam`/`con`/`fun` [declaration](@ref decl) to recurse.
- Application is written by juxtaposition.
- `e @ e` passes an explicit implicit argument.
- `ret p = callee $ arg; body` binds the result of a continuation-style call and continues with `body`.

#### Products {#prod}

```ebnf
e     ::= "[" tlist? "]"
       |  "(" (e ("," e)*)? ")"
       |  "«" shape ";" e "»"
       |  "‹" shape ";" e "›"
       |  e "#" e
       |  e "#" I
       |  e ("#" e)+ "←" e

shape ::= arity ("," arity)*
arity ::= e
       |  I ":" e
```

- `[ ... ]` is a sigma expression unless it is immediately followed by `as` or `→`, in which case it is parsed as the domain of a dependent function type.
- `( ... )` is a tuple expression.
  @note There are no parenthesized/grouping expressions in Mim.
  Instead, you use a 1-tuple.
  A 1-tuple always degrades to its sole element, giving the effect of a parenthesized expression.
- `« ... ; ... »` builds an array.
- `‹ ... ; ... ›` builds a pack.
  Without the `;` both are a [singleton](@ref single) instead.
- A `shape` lists one or more comma-separated dimensions, as in `«i: m, j: n; body»`, and each dimension may optionally be named.
  All of them fuse into **one** array of shape `(m, n)`, whose index binds every axis at once, so `t#i#j` and `t#(i, j)` are the same Extract - except through a [mutable sigma](@ref mutsigma) of one component.
  The exception is a dimension whose extent depends on an earlier index, as in `«i: n, j: s#i; body»`: a shape lists extents, not functions of preceding indices, so such a nest stays one array per axis.
- `e#e` extracts a component by index, `e#I` by [field name](@ref field).
- `tuple#index ← value` yields a **new** aggregate with `index` replaced by `value`; it does not mutate `tuple`.
  A `#` on the left is mandatory: without a component to update there is nothing to insert into.
- `←` updates the component at the _whole_ `#`-path, so `t#i#j#k ← v` denotes the outer aggregate `t`.
  The path extends leftward through `#` and stops at the first expression that is not itself a `#`.
  - `( ... )` builds a tuple (see above), so it ends a path.
    Thus, `(t#i)#j ← v` denotes `t#i` instead.
  - A `let` ends a path the same way: `let row = t#i; row#j ← v` also denotes `t#i`.
  - `←` binds weaker than application, so `f t#i ← v` is `(f t#i) ← v` - write `f (t#i ← v)`.

#### Dependent Tuple Types {#mutsigma}

A sigma is a **dependent tuple type** as soon as one of its components mentions a name to its left:

```mim
let sigma = [n: Nat, «n; Nat»];
```

The components refer to that name through a var of the sigma itself, so such a sigma is a _mutable_: it is built empty and filled in afterwards.
Where no component uses that var, the sigma is the structural one after all - `[i: Nat, j: Nat]` is the very same type as `«2; Nat»`.

A `rec` declaration additionally puts the declared name in scope inside the body, which is the only way for a type to mention itself:

```mim
rec Node = [val: I32, next: mem.Ptr0 Node];
```

`rec` also keeps the sigma mutable unconditionally.

@note "mutable" says how such a sigma is constructed, not that anything about it may be changed later.
A mutable is not hash-consed, so every occurrence is a node of its own, but it is still checked structurally: a second declaration of the same layout is alpha-equivalent to the first, and the two are interchangeable.

A mutable sigma of exactly one component stays a genuine 1-tuple; only `rec` builds one, since a lone component has nothing to its left to depend on.
Everywhere else a one-element aggregate degrades to its sole element - `[T]` and `«1; T»` are `T`, and `(x)` is `x` - which makes `#0₁` a no-op.
Here it is a real Extract, and that is the one exception to `t#i#j` ≡ `t#(i, j)`: a fused index folds its size-1 axes away, while the `#`-chain keeps them.

```mim
rec One = [x: Nat];
lam f (t: «2; One»): Nat = t#1₂#0₁;    // a real Extract: reads `x` out of the 1-tuple
lam g (t: «2; One»): One = t#(1₂, 0₁); // the `0₁` axis folds away, so this is just `t#1₂`
```

`←` is unaffected: writing the component rebuilds the whole 1-tuple either way, so `t#(1₂, 0₁) ← v` and `t#1₂#0₁ ← v` do agree.

#### Unions

```ebnf
e   ::= e "∪" e
     |  e "inj" e
     |  "match" e "with" "|"? p "=>" e ("|" p "=>" e)*
```

- `e ∪ e` forms a union type.
- `e inj e` injects a value into a union type.
- `match e with | p => e | ...` eliminates a union value.

#### Variants {#variant}

```ebnf
e   ::= "|" (I (":" e)? ("|" I (":" e)?)*)?
arm ::= I p? "=>" e
```

- `| I₀: e₀ | ... | Iₙ₋₁: eₙ₋₁` forms a variant type: a sum whose cases are _positional_.
  A constructor without `: e` carries `[]`, and a lone `|` is the empty variant.
- Unlike `∪`, nothing is sorted, deduplicated, or flattened: `| A | B | C` has three cases where `[] ∪ [] ∪ []` is just `[]`, and `| A: Nat | B: Nat` keeps both.
- A variant is a type like any other and may be anonymous; `rec` makes it recursive, and `and` mutually recursive.
- Its last payload extends as far right as it can, and `|` never starts an application argument: write `f (| A | B)`, and parenthesize a variant inside a `match` arm.
- `T#I` or `T#n` on a variant type `T` selects a case by constructor name or by index, counting from `0`.
  That is a value for a `[]` payload, and a function from the payload into `T` otherwise.
- In a `match` on a variant, each arm names a constructor, optionally followed by a pattern for its payload.
  Every constructor needs an arm; a second arm for the same one is unreachable and warned about.
- Constructor names are looked up in the scrutinee's _type_, so any scrutinee works, not just an annotated variable.
  Variants are structural, so two of the same shape are the same type; where they put a name at different positions, it is ambiguous and needs an index.

```mim
rec List = | Nil | Cons: [Nat, List];

lam len (l: List): Nat =
    match l with
        | Nil         => 0
        | Cons (_, t) => core.nat.add (1, len t);

let xs = List#Cons (1, List#Nil);
let ys = List#1 (2, xs); // the same constructor, by index
```

#### Singletons {#single}

```ebnf
e   ::= "«" e "»"
     |  "‹" e "›"
     |  "#" e
```

- `« e »` is the singleton type whose sole inhabitant is `e`.
  It sits one level above `e`, so `«23»` is a type and `«Nat»` a kind; `«Univ»` does not exist.
- `‹ e ›` introduces a value of that type and the prefix `#` eliminates it again, so `#‹e›` is `e`.
- `#` recovers the inhabitant from the type alone: `#x` is `e` for every `x: «e»`.
- The `;` is what tells an [array or pack](@ref prod) from a singleton - `«n; T»` is an array, `«T»` a singleton.
- Because `#` is also the [Extract](@ref prod) operator, it is a prefix only where an expression starts:
  `f #x` extracts rather than applies, so pass a singleton as `f (#x)`.

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

  ```mim
  plugin core;

  let `+ = core.nat.add;
  let x = 2 + 3;
  ```

- `*` doubles as the multiplication operator, so `f *` is a multiplication and not an application of `f` to `Type (0:Univ)`; write `f (*)` for the latter.

#### Local Declaration Blocks

```ebnf
e   ::= e "where" d* "end"
```

`where` attaches a local declaration block to an already parsed expression.
`where` blocks bind more weakly than the other infix expression forms.

### Precedence {#prec}

Parser and dumper share one ladder of precedence levels, listed here from strongest to weakest binding.
_Assoc_ is left-, right-, or non-associative; chaining a non-associative operator, as in `a == b == c`, is an error - parenthesize one side.
`Pi`, `Bot`, and `Err` are _pseudo levels_: they name no syntax at all and only ever bound how far a nested expression may extend.
A `+`/`-` sign is part of the [literal](@ref lit) rather than an operator and so has no level of its own.

|   # | Level     | Assoc | Operators                            | Notes                                                                                                                             |
| --: | --------- | :---: | ------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------- |
|   1 | `Lit`     |   -   | `L:e`                                | The tightest level. Not an infix operator - the literal parser reads the ascription itself and bounds `e` here.                   |
|   2 | `Extract` | left  | `e#e`, `e#I`                         |                                                                                                                                   |
|   3 | `App`     | left  | `e e`, `e @ e`                       | Application binds tighter than every operator. Also bounds the `e` in `Type e` and `Rule e`.                                      |
|   4 | `Prefix`  |   -   | `#e`                                 | The only prefix level. Looser than application, so `#f x` is `#(f x)`.                                                            |
|   5 | `Shift`   | left  | `e << e`, `e >> e`                   | Tighter than `*`, as in Lean and OCaml - not the C position.                                                                      |
|   6 | `Mul`     | left  | `e * e`, `e / e`, `e % e`            |                                                                                                                                   |
|   7 | `Add`     | left  | `e + e`, `e - e`                     |                                                                                                                                   |
|   8 | `Rel`     | none  | `e < e`, `e <= e`, `e > e`, `e >= e` |                                                                                                                                   |
|   9 | `Eq`      | none  | `e == e`, `e != e`                   |                                                                                                                                   |
|  10 | `Pi`      |   -   | _pseudo_                             | Bounds the domain of a `λ`/`Fn`/`t → e` binder so it stops before the `→`. A `Cn`-style binder has no `→` and uses `Bot` instead. |
|  11 | `Arrow`   | right | `e → e`                              | Also bounds the codomain after a `→`.                                                                                             |
|  12 | `Union`   | left  | `e ∪ e`                              |                                                                                                                                   |
|  13 | `Inj`     | right | `e inj e`                            | Weaker than `∪`, so `x inj A ∪ B` is `x inj (A ∪ B)`.                                                                             |
|  14 | `Ins`     | right | `e("#"e)+ ← e`                       | Also bounds a declaration's `: codom` slot, which ends at `=` and so takes everything short of a `where`.                         |
|  15 | `Where`   | left  | `e where d* end`                     | The loosest surface operator.                                                                                                     |
|  16 | `Bot`     |   -   | _pseudo_                             | A complete expression; the default bound, and the only one a trailing `where` fits into.                                          |
|  17 | `Err`     |   -   | _pseudo_                             | Below everything; the parser's "no operator seen yet" sentinel.                                                                   |

## Summary: Functions and Types

Mim uses different surface syntax for declarations, expressions, and types:

| Declaration | Expression | Type |
| ----------- | ---------- | ---- |
| `lam`       | `λ` / `lm` | `→`  |
| `con`       | `cn`       | `Cn` |
| `fun`       | `fn`       | `Fn` |

### Declarations

The following declarations are equivalent:

```mim
lam f1 (T: *) ((x y: T), return: T → ⊥)@ff: ⊥ = return x;
con f2 (T: *) ((x y: T), return: Cn T)        = return x;
fun f3 (T: *)  (x y: T): T                    = return x;
```

A partial-evaluation filter defaults to `tt`, except on the last domain of a `con`, `cn`, `fun`, or `fn`, where it defaults to `ff`.

### Expressions

The following expressions are equivalent.
Because they are bound by `let`, they behave like the declarations above - except that `f` is _not_ in scope inside the body, so they cannot recurse:

```mim
let f1 =  λ (T: *) ((x y: T), return: T → ⊥)@ff: ⊥ = return x;
let _  = lm (T: *) ((x y: T), return: T → ⊥)@ff: ⊥ = return x;
let f2 = cn (T: *) ((x y: T), return: Cn T)        = return x;
let f3 = fn (T: *)  (x y: T): T                    = return x;
```

### Applications

The following applications of `f` are equivalent, where `g` is a continuation that consumes the result:

```mim
fun f (T: *)  (x y: T): T = return x;

fun test1 (): Nat =
    f Nat ((23, 42), cn res: Nat = return res);

fun test2 (): Nat =
    ret res = f Nat $ (23, 42);
    return res;
```

### Function Types

The following types are equivalent and describe the type of `f` above:

```mim
let _ = [T: *] →    [[T, T], T → ⊥] → ⊥
let _ = [T: *] → Cn [[T, T], Cn T]
let _ = [T: *] → Fn  [T, T] → T
```

## Scoping

Mim uses [lexical scoping](<https://en.wikipedia.org/wiki/Scope_(computer_science)#Lexical_scope>).
Unless noted otherwise, all names live in the same scope.
A file is bound in isolation: it never sees the scope of whoever imports it.

### Underscore

The symbol `_` is special: it never binds an entity.
As a consequence, `_` may appear repeatedly in the same scope without conflict, but any use of `_` as a reference is a scoping error.

### Annex {#annex}

An `anx` declaration is an ordinary member of its enclosing module, found by the same path resolution as any other member.
Its plugin-qualified name (`plugin.tag` or `plugin.tag.sub`, derived from `mod` nesting) is additionally registered in a global by-name table (@ref mim::Annex) that tools such as `compile.named` and plugin bootstrap use to look an annex up directly by that name, independent of the surrounding modules.

### Field Names of Sigmas {#field}

Named components of a sigma are available for extracts and inserts.
A structural sigma keeps no names in the IR, so the frontend records them per shape: once `[i: Nat, j: Nat]` is declared, every `«2; Nat»` answers to `i` and `j`.
A name that another sigma of the same shape puts at a different position is ambiguous there; select the component by index instead.
A [mutable sigma](@ref mutsigma) is a node of its own, so its names are never ambiguous.
@note These names take precedence over ordinary lexical names.
In the example below, `i` refers to the field name of `S`, not the `let`-bound variable:

```mim
let i = 1₂;
rec S = [i j: Nat];
lam f (x: S): Nat = x#i;
```

Use parentheses to force the variable interpretation:

```mim
let i = 1₂;
rec S = [i j: Nat];
lam f (x: S): Nat = x#(i);
```

The constructor names of a [variant](@ref variant) behave the same way after `#`, as in `T#Red`.

## Normalizations {#normalization}

Mim nodes are hash-consed and normalized while they are built, so the left-hand sides below never reach the IR - `mim --output-mim` prints the right-hand side.
Every argument is [zonked](@ref mim::Zonker) first, so a resolved `Hole` normalizes like the term it stands for.
A _mutable_ node is built empty and filled in afterwards and is therefore never normalized; only its body is.
While the `World` is frozen, a rule that would have to build a new node bails out instead.

### Aggregates

- `(e)` -> `e`, `[T]` -> `T` - a one-element aggregate degrades to its element; the sole exception is a [mutable sigma](@ref mutsigma) of one component
- `(e, e, e)` -> `‹3; e›`, `[T, T, T]` -> `«3; T»` - uniform elements compress into a pack/array
- `(t#0, t#1, t#2)` -> `t` - η for tuples, provided `t` has the ascribed type
- `⊥:[T, U]` -> `(⊥:T, ⊥:U)`, `⊥:«n; T»` -> `‹n; ⊥:T›` - `⊥`/`⊤` are pushed into aggregates

### Shapes

- `«1; T»` -> `T` - a literal size-1 axis folds out of a shape
- `«2, 0, 3; T»` -> `«2; []»` - a literal `0` extent empties everything below it
- `«a; «b; T»»` -> `«a, b; T»` - a nest of the same kind fuses into one shape, unless the inner extents depend on the outer index

### Extract

- `d#(i, 0₁, k)` -> `d#(i, k)` - a literal size-1 axis folds out of an index, mirroring the shape rule
- `d#i` -> `d` if `i: Idx 1` and `d`'s type is not a mutable sigma of one component
- `d#()` -> `d` - an index that folded away entirely
- `‹i: n; e›#j` -> `[i ↦ j]e`, `(a, b, c)#1` -> `b`
- `t#i#j` -> `t#(i, j)` - the maximal-rank fused Extract is the normal form, as far as `t`'s own shape reaches
- `t#(i, j)` -> `t#i#j` - conversely, for an index reaching past `t`'s own shape into its element type
- `(d#i ← v)#i` -> `v`
- `(d#j ← v)#i` -> `d#i` for literal `i ≠ j`

### Insert

- `d#() ← v` -> `v`, and likewise for an index of `Idx 1` - the write replaces all of `d`
- `(a, b, c)#1 ← x` -> `(a, x, c)`
- `‹4; x›#2 ← y` -> `(x, x, y, x)` - only for a literal arity below `--scalarize-threshold`
- `d#(i, j) ← v` -> `d#i ← ((d#i)#j ← v)` - if the outer write rebuilds an aggregate by the two rules above, or if the index reaches past `d`'s own shape
- `d#i ← ((d#i)#j ← v)` -> `d#(i, j) ← v` - otherwise, the dual of Extract fusion: a read-modify-write chain stays a _single_ write
- `d#i ← d#i` -> `d`
- `(d#i ← y)#i ← v` -> `d#i ← v`

### Application

- `(λ (_: T) = e) a` -> `e` - an immutable `λ` binds no var, so it always β-reduces
- `f x` -> body of `f` if `x` is `f`'s own var - substituting a var by itself is the identity
- `f x` -> `[var ↦ x]`body of `f` if `f`'s [filter](@ref decl) evaluates to `tt` for `x`; nothing is reduced if the filter is `ff`
- an application of an [axiom](@ref decl) runs its normalizer once the curry counter hits `0`
- an application of a `[T: *] → e` with implicit domains inserts a `Hole` per implicit argument

### Unions

- `T ∪ ⊥` -> `T` - the unit of a join is dropped
- `T ∪ ⊤` -> `⊤`
- `A ∪ A` -> `A`; the operands are flattened and sorted, so `∪` is commutative, associative, and idempotent
- an empty join is `⊥`, and a one-element one is its operand
- `x inj T` -> `x` if `T` is not a union type
- `match (T inj x) with ...` -> the arm handling `T` - a constructor fixes the active case
- each case is handled by the **first** arm accepting it, so the arms are *not* sorted; an arm accepts a case if its domain is that case, or a union containing it
- a `match` whose scrutinee is not a union is the degenerate one-case union and reduces right away
- an arm handling no case is dropped; a case handled by no arm is an error

### Variants

- a variant is never sorted, deduplicated, or flattened; an empty or one-case variant stays a variant
- `match (T#i x) with ...` -> the arm for case `i`
- the arms of a `match` on a variant are in the order of its cases, one each

### Singletons

- `#x` -> `e` for every `x: «e»` - the inhabitant is read off the *type*, so the var of a `λ (x: «e»)` never occurs in the body
- `#‹e›` -> `e` - a special case of the above; a singleton elimination is therefore never built

### Universes

- `Type` levels: `l + 1` folds for a literal `l`, and a `UMax` flattens nested maxima, folds their literals into one, and sorts and deduplicates the rest
- the var of a mutable whose var type is `Idx 1` -> `0₁`, and one whose var type is `[]` -> `()`

<div class="section_buttons">

| Previous                           |                                     Next |
| :--------------------------------- | ---------------------------------------: |
| [Command-Line Reference](@ref cli) | [Contributing \& Debugging](@ref coding) |

</div>
