# MimIR

[![Stars](https://img.shields.io/github/stars/mimir/mimir)](https://github.com/mimir/mimir/stargazers)
[![Forks](https://img.shields.io/github/forks/mimir/mimir)](https://github.com/mimir/mimir/fork)
[![Discord](https://img.shields.io/discord/960975142459179068?style=social&logo=discord&logoColor=black)](https://discord.gg/FPp7hdj3fQ)

[![Release](https://img.shields.io/github/v/release/mimir/mimir?style=flat-square&logo=starship&color=blue&label=Release)](https://github.com/mimir/mimir/releases)
[![Docs](https://img.shields.io/badge/Docs-master/v0.1-blue?style=flat-square&logo=gitbook&logoColor=white)](https://mimir.github.io)
[![License](https://img.shields.io/github/license/mimir/mimir?style=flat-square&color=blue&logo=opensourceinitiative&logoColor=white&label=License)](https://github.com/mimir/mimir/blob/master/LICENSE.TXT)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue?style=flat-square&logo=cplusplus)](https://en.wikipedia.org/wiki/C%2B%2B#Standardization)
[![Python](https://img.shields.io/badge/Python-3.10-blue?style=flat-square&logo=python&logoColor=white)](https://www.python.org)

[![Doxygen](https://img.shields.io/github/actions/workflow/status/mimir/mimir/doxygen.yml?style=flat-square&logo=doxygen&logoSize=auto&label=&labelColor=555&branch=master)](https://github.com/mimir/mimir/actions/workflows/doxygen.yml?query=branch%3Amaster)
[![Linux](https://img.shields.io/github/actions/workflow/status/mimir/mimir/linux.yml?style=flat-square&logo=linux&label=Linux&logoColor=white&branch=master)](https://github.com/mimir/mimir/actions/workflows/linux.yml?query=branch%3Amaster)
[![macOS](https://img.shields.io/github/actions/workflow/status/mimir/mimir/macos.yml?style=flat-square&logo=apple&label=macOS&branch=master)](https://github.com/mimir/mimir/actions/workflows/macos.yml?query=branch%3Amaster)
[![Windows](https://img.shields.io/github/actions/workflow/status/mimir/mimir/windows.yml?style=flat-square&label=⊞%20Windows&branch=master)](https://github.com/mimir/mimir/actions/workflows/windows.yml?query=branch%3Amaster)

[TOC]

**MimIR** is a pure, graph-based, higher-order intermediate representation rooted in the **Calculus of Constructions**.
MimIR provides:

- **Dependent types**, **parametric polymorphism**, and **higher-order functions** out of the box
- **Extensible plugins** for domain-specific axioms, types, normalizers, and code generation
- **SSA without dominance**: a scopeless IR for higher-order programs based on free-variable nesting
- A **sea-of-nodes** style IR with on-the-fly normalization, type checking, and partial evaluation

MimIR is well suited for DSL compilers, tensor compilers, automatic differentiation, regex engines, and other systems that need high-performance code from high-level abstractions.

## 💡 Why MimIR?

| Feature                                                                             | LLVM                         | MLIR                                   | MimIR                                                                                                                                                                                |
| ----------------------------------------------------------------------------------- | ---------------------------- | -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [Higher-order functions](https://en.wikipedia.org/wiki/Higher-order_function)       | ❌                           | ⚠️ (regions only)                      | ✅ (first-class functions)                                                                                                                                                           |
| [Type operators](https://en.wikipedia.org/wiki/Type_constructor)                    | ❌                           | ❌                                     | ✅                                                                                                                                                                                   |
| [Parametric polymorphism](https://en.wikipedia.org/wiki/System_F)                   | ❌                           | ❌                                     | ✅                                                                                                                                                                                   |
| [Higher-kinded polymorphism](https://en.wikipedia.org/wiki/System_F#System_F%CF%89) | ❌                           | ❌                                     | ✅                                                                                                                                                                                   |
| [Dependent types](https://en.wikipedia.org/wiki/Calculus_of_constructions)          | ❌                           | ❌                                     | ✅                                                                                                                                                                                   |
| Semantic extensibility                                                              | ❌                           | 🔧 (dialect-specific C++ semantics)    | ✅ (typed axioms)                                                                                                                                                                    |
| Program representation                                                              | CFG + <br> instruction lists | CFG / regions + <br> instruction lists | Arbitrary expressions <br> (direct style + [CPS](https://en.wikipedia.org/wiki/Continuation-passing_style))                                                                          |
| Structural foundation                                                               | CFG + <br> dominance         | CFG / regions + <br> dominance         | Free variables + nesting                                                                                                                                                             |
| DSL embedding / <br> semantics retention                                            | ⬇️ Low                       | ➡️ Medium <br> (dialects, lowering)    | ⬆️ High ([CC](https://en.wikipedia.org/wiki/Calculus_of_constructions), [partial evaluation](https://en.wikipedia.org/wiki/Partial_evaluation), typed axioms, normalizers, lowering) |

@note The table compares native IR-level support and representation, not what can be emulated via custom IR extensions, closure conversion, lowering, or external analyses.

## 🚀 Quick Start

```sh
git clone --recursive git@github.com:mimir/mimir.git
cd mimir
cmake -S . -B build -DBUILD_TESTING=ON -DMIM_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

### 📦 Install (Optional)

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DMIM_BUILD_EXAMPLES=ON -DCMAKE_INSTALL_PREFIX=/my/local/install/prefix
cmake --build build -j$(nproc) --target install
```

See the full [build options](@ref building) in the docs.
You can find the rest of the documentation [here](file:///home/roland/mimir/mimir/build/html/usergroup0.html)

## 🔥 Key Innovations

### 🧩 Plugins

Declare new types, operations, and normalizers in a single `.mim` file.
C++ provides the heavy lifting: optimization, lowering, and code generation.

Explore the [Plugin Registry](https://mimir.github.io/plugins) to discover and share community-developed plugins.

### 🌊 Sea of Nodes

MimIR uses a [sea-of-nodes-style](https://github.com/SeaOfNodes) program graph and extends it to the Calculus of Constructions with higher-order functions, polymorphism, and dependent types.
MimIR hits the [**sweet spot**](@ref mut) between a fully mutable IR, which is easy to construct, and a fully immutable IR:

- **Non-binder expressions are immutable**:

  [Hash-consing](https://en.wikipedia.org/wiki/Hash_consing), normalization, type checking, and partial evaluation happen **automatically** during graph construction.

- **Binders are mutable where needed**:

  They support variables and recursion by “tying the knot” through in-place mutation.

- **Terms and types share one graph**:

  Terms, types, and type-level computations all live in the same program graph as ordinary expressions.

### 🪾 SSA without Dominance

Forget CFG dominance.
MimIR uses free-variable nesting:

- **Free variables** replace dominance; the **nesting tree** replaces the dominator tree
- Free-variable queries “just work”:

  ```c++
  if (expr->free_vars().contains(x)) /*x free in expr*/
  if (expr->free_vars().has_intersection(xyz)) /*x, y, or z free in expr*/
  ```

  This is always correct.
  MimIR maintains free-variable information **lazily**, **locally**, and **transparently**: results are computed on demand, memoized, and invalidated only where needed.

- Data dependencies remain precise, even for higher-order code
- Loop peeling and unrolling reduce to simple β-reduction
- Mutual recursion and higher-order functions are handled naturally

## 🐉 Naming: MimIR vs. Mim

**MimIR** is a recursive acronym for _MimIR is my Intermediate Representation_.

In Norse mythology, [Mímir](https://en.wikipedia.org/wiki/M%C3%ADmir) was a being of immense wisdom.
After being beheaded in the Æsir–Vanir War, Odin preserved his head, which continued to speak secret knowledge and offer counsel.

Today, **you** have Mímir's head at your fingertips.

- **MimIR** refers to the core graph-based intermediate representation and its C++ API.
- **Mim** is a lightweight textual representation of MimIR.
  It is not a full-featured programming language, but provides enough syntactic sugar to concisely express polymorphic and dependent types (including type-level dependencies introduced by many type variables).
  Mim is mainly intended for defining plugin interfaces and writing small test cases.

Throughout the codebase, we consistently use `mim` / `MIM` for namespaces, macros, CMake variables, and related identifiers.

**Acknowledgments**:
We gratefully acknowledge [Alex Wendland](https://github.com/AlexWendland) and the other maintainers of the former `MimIR` GitHub organization for kindly making the organization name available for this project.
The previous organization has been renamed to [`mimir-depricated`](https://github.com/mimir-depricated).

## 💬 Community

- 💬 **Discord** → [Join the chat](https://discord.gg/FPp7hdj3fQ)
- 📚 **Documentation** → <https://mimir.github.io>
- 💻 **Examples** → [`examples/`](https://github.com/mimir/mimir/tree/master/examples) and [`lit/`](https://github.com/mimir/mimir/tree/master/lit)

**Ready to build the next generation of DSL compilers?**

[⭐ Star MimIR on GitHub](https://github.com/mimir/mimir), join Discord, and let's make high-performance DSLs easy.

## ⚖️ License

MimIR is licensed under the [MIT License](https://github.com/mimir/mimir/blob/master/LICENSE.TXT).

## 📖 Publications

- **SSA without Dominance for Higher-Order Programs** <br>
  Roland Leißa, Johannes Griebler <br>
  [![PLDI 2026](https://img.shields.io/badge/PLDI-2026-blue?style=flat-square)](https://pldi26.sigplan.org)
  [![ACM](https://img.shields.io/badge/ACM-10.1145/3808286-blue?style=flat-square&logo=acm)](https://doi.org/10.1145/3808286)
  [![arXiv](https://img.shields.io/badge/arXiv-10.48550/arXiv.2604.09961-blue?style=flat-square&logo=arxiv)](https://doi.org/10.48550/arXiv.2604.09961)
  [![zenodo](https://img.shields.io/badge/-10.5281%2Fzenodo.19069678-blue?style=flat-square&logo=zenodo&logoColor=white&labelColor=555&logoSize=auto)](https://doi.org/10.5281/zenodo.19069678)
  <br><br>

- **MimIrADe: Automatic Differentiation in MimIR** <br>
  Marcel Ullrich, Sebastian Hack, Roland Leißa <br>
  [![CC 2025](https://img.shields.io/badge/CC-2025-blue?style=flat-square)](https://conf.researchr.org/home/CC-2025)
  [![PDF](https://img.shields.io/badge/PDF-grey?style=flat-square&logo=readthedocs)](https://raw.githubusercontent.com/leissa/leissa/main/uhl25.pdf)
  [![ACM](https://img.shields.io/badge/ACM-10.1145/3708493.3712685-blue?style=flat-square&logo=acm)](https://dl.acm.org/doi/abs/10.1145/3708493.3712685)
  [![zenodo](https://img.shields.io/badge/-10.5281/zenodo.14681109-blue?style=flat-square&logo=zenodo&logoColor=white&labelColor=555&logoSize=auto)](https://doi.org/10.5281/zenodo.14681109)
  [![dblp](https://img.shields.io/badge/dblp-grey?style=flat-square&logo=dblp)](https://dblp.uni-trier.de/rec/conf/cc/UllrichHL25.html?view=bibtex)
  <br><br>

- **MimIR: An Extensible and Type-Safe Intermediate Representation for the DSL Age** <br>
  Roland Leißa, Marcel Ullrich, Joachim Meyer, Sebastian Hack <br>
  [![POPL 2025](https://img.shields.io/badge/POPL-2025-blue?style=flat-square)](https://conf.researchr.org/home/POPL-2025)
  [![PDF](https://img.shields.io/badge/PDF-grey?style=flat-square&logo=readthedocs)](https://raw.githubusercontent.com/leissa/leissa/main/lumh25.pdf)
  [![ACM](https://img.shields.io/badge/ACM-10.1145/3704840-blue?style=flat-square&logo=acm)](https://doi.org/10.1145/3704840)
  [![arXiv](https://img.shields.io/badge/arXiv-10.48550/arXiv.2411.07443-blue?style=flat-square&logo=arxiv)](https://doi.org/10.48550/arXiv.2411.07443)
  [![zenodo](https://img.shields.io/badge/-10.5281/zenodo.19069678-blue?style=flat-square&logo=zenodo&logoColor=white&labelColor=555&logoSize=auto)](https://doi.org/10.5281/zenodo.19069678)
  [![YouTube](https://img.shields.io/badge/YouTube-grey?style=flat-square&logo=youtube)](https://youtu.be/2zKUa6b9XYc?si=3ZX68gEHarsCsO-R)
  [![dblp](https://img.shields.io/badge/dblp-grey?style=flat-square&logo=dblp)](https://dblp.uni-trier.de/rec/journals/pacmpl/LeissaUMH25.html?view=bibtex)
