# Mim Language Reference {#langref}

[TOC]

## Notation

This document uses a lightweight [EBNF](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form)-style notation.

```ebnf
"a"        literal terminal token a
[a b]      one of a or b
[a-c]      character range from a to c
x*         zero or more repetitions of x
x+         one or more repetitions of x
x?         optional x
x ("," x)* ","?
           comma-separated list of zero or more x, with optional trailing comma
```

## Lexical Structure {#lex}

Mim source files are [UTF-8](https://en.wikipedia.org/wiki/UTF-8) encoded and are [lexed](https://en.wikipedia.org/wiki/Lexical_analysis) from left to right.
The lexer uses [maximal munch](https://en.wikipedia.org/wiki/Maximal_munch), so ambiguities are resolved by taking the longest matching token.
For example, `<<<` is tokenized as `<<` followed by `<`.

### Terminals {#terminals}

The grammar refers to *primary terminals*.
Some tokens also have ASCII-only spellings, called *secondary terminals*, that denote the same lexical token.
For example, `λ` and `lm` are lexically equivalent.

#### Primary Terminals

```text
( ) [ ] { } ⦃ ⦄
‹ › « »
→ => ⊥ ⊤ * □ λ
= , ; . : @ $ # | ∪
<eof>
```

`%` is only part of annex names and is not a standalone token.

#### Secondary Terminals

```text
< > << >>
-> bot top lm insert
```

`⟨`, `⟩`, `⟪`, and `⟫` may be used as alternatives for `‹`, `›`, `«`, and `»`.

#### Keywords

```text
Bool Cn Fn I1 I8 I16 I32 I64 Idx Nat Rule Type Univ
and as axm ccon cfun cn con end extern ff fn fun
i1 i8 i16 i32 i64 import inj ins lam let match module
norm plugin rec ret rule tt when where with
```

`module` is currently reserved by the lexer even though it does not introduce a surface construct in the grammar below.

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
I      ::= sym
A      ::= "%" sym "." sym ("." sym)?
L      ::= dec+
         |  "0" ["bB"] bin+
         |  "0" ["oO"] oct+
         |  "0" ["xX"] hex+
         |  sign dec+
         |  sign "0" ["bB"] bin+
         |  sign "0" ["oO"] oct+
         |  sign "0" ["xX"] hex+
         |  sign? dec+ eE sign dec+
         |  sign? dec+ "." dec* (eE sign dec+)?
         |  sign? dec* "." dec+ (eE sign dec+)?
         |  sign? "0" ["xX"] hex+ pP sign dec+
         |  sign? "0" ["xX"] hex+ "." hex* pP sign dec+
         |  sign? "0" ["xX"] hex* "." hex+ pP sign dec+
X_n    ::= dec+ sub+
         |  dec+ "_" dec+
C      ::= "'" (ascii_char | esc) "'"
S      ::= "\"" (ascii_string_char | esc)* "\""
```

Here `I` is an identifier, `A` is an [annex](@ref mim::Annex) name, `L` is a numeric literal, `X_n` is an index literal of type `Idx n`, `C` is a character literal, and `S` is a string literal.

The shorthand symbols used above are:

```ebnf
bin    ::= [0-1]
oct    ::= [0-7]
dec    ::= [0-9]
sub    ::= [₀-₉]
hex    ::= [0-9a-fA-F]
eE     ::= ["eE"]
pP     ::= ["pP"]
sign   ::= ["+-"]
sym    ::= [_a-zA-Z] [._0-9a-zA-Z]*
esc    ::= one of: \' \" \0 \a \\ \b \f \n \r \t \v
```

Character and string literals only admit ASCII payload characters plus the escapes listed above.

## Comments

Mim supports `/* ... */` multi-line comments, `// ...` single-line comments, and `/// ...` comments that are forwarded to generated [Markdown](https://www.doxygen.nl/manual/markdown.html) output.
`/* ... */` comments are not nested.
For `///` comments, a line of the form `/// text` contributes `text` directly to the Markdown output.
Other `///` forms are emitted verbatim inside a [fenced code block](https://www.doxygen.nl/manual/markdown.html#md_fenced).

## Grammar {#grammar}

Mim is defined by a [context-free grammar](https://en.wikipedia.org/wiki/Context-free_grammar).
Its terminals are the lexical elements defined above.
The start symbol is `m` for *module*.

The main nonterminals used below are:

```text
m   module
d   declaration
p   ()-style pattern
b   []-style pattern
e   expression
```

### Module {#module}

```ebnf
m   ::= dep* d*
dep ::= "import" I ";"
     |  "plugin" I ";"
```

A module consists of zero or more imports or plugins followed by zero or more declarations.

- `import foo;` resolves the module name `foo` through the search path.
- If the resolved file name has no extension, `.mim` is appended.
- `plugin foo;` first loads the plugin `foo` and then imports the module with the same name.

### Declarations {#decl}

Mim accepts the following declaration families.

```text
let p = e
let A = e

lam|con|fun [extern] n dom* [: e] = e
ccon|cfun I b [: e]

rec n [: e] = e
and n [: e] = e
and lam|con|fun [extern] n dom* [: e] = e

axm A [(tag (= alias)*)*] : e [, normalizer] [, curry] [, trip]

rule|norm n p : e [when e] => e
```

Here `n` is either an identifier or an annex name.

- `let` introduces a binding pattern.
- `lam`, `con`, and `fun` declare lambdas, continuations, and returning continuations.
- `extern` may appear on `lam`, `con`, and `fun` declarations.
- Each domain in a `lam`-style declaration may be followed by a filter introduced with `@`.
- `ccon` and `cfun` declare external C continuations and C functions.
- `rec` starts a recursive declaration group, and `and` extends the same group.
- After `and`, the next declaration may be another `rec`-style binding or an explicit `lam`, `con`, or `fun` declaration.
- `axm` declares an axiom and may carry tag aliases, a normalizer, and curry or trip metadata.
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

```mim
let (a, b, c) as abc = (1, 2, 3);
```

This binds `a`, `b`, and `c` to the tuple elements and `abc` to the whole tuple.

- Bracket-style patterns may also contain general expressions.
- This is what makes forms such as `[T: *] → T` and `Cn [mem: %mem.M 0, I32]` legal.
- `let` and `ret` allow rebinding of an existing name.

This is especially useful for state-threading style code:

```mim
let (mem, ptr) = %mem.alloc (I32, 0) mem;
let mem        = %mem.store (mem, ptr, 23:I32);
let (mem, val) = %mem.load (mem, ptr);
```

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

#### Literals and Basic Forms

```ebnf
e   ::= L (":" e)?
     |  X_n
     |  "ff"
     |  "tt"
     |  C
     |  S
     |  "⊥" (":" e)?
     |  "⊤" (":" e)?
     |  n
     |  "{" d* e "}"
     |  "⦃" e "⦄"
```

- A numeric, character, string, `⊥`, or `⊤` literal may carry an explicit type ascription.
- Without an explicit type, numeric literals default to `Nat`.
- Without an explicit type, `⊥` and `⊤` default to `*`.
- `{ d* e }` is a declaration expression whose result is the final expression `e`.
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

#### Products, Sequences, Unions, and Matching

```ebnf
e   ::= "[" ... "]"
     |  "(" (e ("," e)* ","?)? ")"
     |  "«" arity (";" e) "»"
     |  "‹" arity (";" e) "›"
     |  e "#" e
     |  e "#" I
     |  "ins" "(" e "," e "," e ")"
     |  e "∪" e
     |  e "inj" e
     |  "match" e "with" ("|"? p "=>" e)+

arity ::= e
       |  I ":" e
```

- `[ ... ]` is a sigma expression unless it is immediately followed by `as` or `→`, in which case it is parsed as the domain of a dependent function type.
- `( ... )` is a tuple expression.
- `« ... ; ... »` builds an array.
- `‹ ... ; ... ›` builds a pack.
- Each array or pack dimension may optionally be named, as in `«n: len; body»`.
- `e # i` or `e # e` extracts a component.
- `ins(tuple, index, value)` inserts a value into a tuple-like aggregate.
- `e ∪ t` forms a union type.
- `e inj t` injects a value into a union type.
- `match e with | p => e | ...` eliminates a union value.

#### Local Declaration Blocks

```ebnf
e   ::= e "where" d* "end"
```

`where` attaches a local declaration block to an already parsed expression.
`where` blocks bind more weakly than the other infix expression forms.

### Precedence

The current parser uses the following precedence, from strongest to weakest binding:

```text
1.  L : e                  literal and token-local type ascription
2.  e # e                  extract
3.  e ∪ e                  union
4.  e e, e @ e             application
5.  e inj e                injection
6.  e → e                  arrow
7.  e where d* end         local declaration block
```

- Extract, union, and application associate left-to-right.
- `inj` and `→` associate right-to-left.
- `where` is the loosest surface operator.

## Summary: Functions and Types

Mim uses different surface syntax for declarations, expressions, and types:

```text
Declaration   Expression   Type
lam           λ / lm       ->
con           cn           Cn
fun           fn           Fn
```

### Declarations

The following declarations are equivalent:

```mim
lam f(T: *)((x y: T), return: T → ⊥)@ff: ⊥ = return x;
con f(T: *)((x y: T), return: Cn T)        = return x;
fun f(T: *) (x y: T): T                    = return x;
```

Partial-evaluation filters default to `tt`, except for `con`, `cn`, `fun`, and `fn`.

### Expressions

The following expressions are equivalent.
Because they are bound by `let`, they behave like the declarations above:

```mim
let f =  λ (T: *) ((x y: T), return: T → ⊥)@ff: ⊥ = return x;
let f = lm (T: *) ((x y: T), return: T → ⊥)   : ⊥ = return x;
let f = cn (T: *) ((x y: T), return: Cn T)        = return x;
let f = fn (T: *)  (x y: T): T                    = return x;
```

### Applications

The following applications of `f` are equivalent:

```mim
f Nat ((23, 42), cn res: Nat = use(res))
ret res = f Nat $ (23, 42); use(res)
```

### Function Types

The following types are equivalent and describe the type of `f` above:

```mim
[T: *] →    [[T, T], T → ⊥] → ⊥
[T: *] → Cn [[T, T], Cn T]
[T: *] → Fn  [T, T] → T
```

## Scoping

Mim uses [lexical scoping](<https://en.wikipedia.org/wiki/Scope_(computer_science)#Lexical_scope>).
Unless noted otherwise, all names live in the same namespace.

### Underscore

The symbol `_` is special: it never binds an entity.
As a consequence, `_` may appear repeatedly in the same scope without conflict, but any use of `_` as a reference is a scoping error.

### Annex

Annex names live in a separate global namespace.

### Field Names of Sigmas

Named elements of mutable sigma types are available for extracts and inserts.
@note These names take precedence over ordinary lexical names.
In the example below, `i` refers to the field name of `X`, not the `let`-bound variable:

```mim
let i = 1_2;
[i: Nat, j: Nat]::X → f X#i;
```

Use parentheses to force the variable interpretation:

```mim
let i = 1_2;
[i: Nat, j: Nat]::X → f X#(i);
```
