# Plugins {#plugins}

[TOC]

Check out the [demo](@ref demo) plugin for a minimal example.
It uses our custom [`add_mim_plugin`](@ref add_mim_plugin_cmake) CMake command.
A plugin generally consists of two halves with the same name: a `<plugin>.mim` file that declares the public annexes and a shared library that registers the runtime behavior.

Plugin names may only contain letters, digits, and underscores, and are limited to 8 characters.

## Example: demo Plugin

The [demo](@ref demo) plugin at `src/mim/plug/demo/` is the minimal, complete example every in-tree plugin follows.
It consists of five checked-in files:

- `CMakeLists.txt` — a single `add_mim_plugin` call
- `demo.mim` — the public annex surface
- `demo.h` — the public header, over in `include/mim/plug/demo/`
- `demo.cpp` — the plugin's entry point
- `normalizers.cpp` — the normalizer implementations

**`CMakeLists.txt`**:

\include "src/mim/plug/demo/CMakeLists.txt"

**`demo.mim`**:

\include "src/mim/plug/demo/demo.mim"

Doc comments (`///`) are ordinary Doxygen-flavored Markdown (headings, `[TOC]`, `@see`, ...); everything else is plain Mim syntax declaring the annex itself.
Here `%demo.const_idx` is a single axiom with no subtags, and `normalize_const` names the C++ function that evaluates it.
Building the plugin auto-generates a C++ header and a Python module from this very file, and turns its doc comments into the plugin's Doxygen page — see [Generated Interfaces](@ref plugin_codegen) below, since this applies to every plugin, not just `demo`.

\anchor demo_h
**`demo.h`**:

\include "include/mim/plug/demo/demo.h"

It is a header, not a translation unit of its own: `demo.cpp` and `normalizers.cpp` `#include` it, and it in turn `#include`s the generated header it wraps.
See [Generated Header](@ref plugin_h) below for why it is just this two-line indirection.

**`demo.cpp`**:

\include "src/mim/plug/demo/demo.cpp"

The function `mim_get_plugin` is the single entry point [`Driver`](@ref mim::Driver) looks up (via `dlopen`/`dlsym`) when a `plugin demo;` directive or `-p demo` loads the shared module.
It returns a [`mim::Plugin`](@ref mim::Plugin) record: the plugin's name, the `MIM_VERSION` it was built against (checked against the loading `mim` binary), the `register_normalizers` function that `normalizers.cpp` defined via `MIM_demo_NORMALIZER_IMPL`, an optional phase-registration callback (`{}` here, since `demo` defines no phases), and the [`-X` arguments](@ref mim::PluginArg) it understands (none here).
It is a POD, so every field must be spelled out - `{}` for the ones a Plugin does not use.

**`normalizers.cpp`**:

\include "src/mim/plug/demo/normalizers.cpp"

Normalizers usually obtain the owning [`World`](@ref mim::World) from one of their arguments — here `type->world()` — and build the replacement directly in that world, without ever materializing the [`App`](@ref mim::App) node it replaces; that is why `normalize_const` just returns the literal `42`.
If a normalizer cannot do anything meaningful, it should return `{}`/`nullptr`: [`World::app`](@ref mim::World::app) treats a null return as "give up" and falls back to constructing the real `App` node from the original arguments instead.
The macro `MIM_demo_NORMALIZER_IMPL` stems from the generated header (see [below](@ref plugin_codegen)) that expands into a full definition of `register_normalizers`, wiring `normalize_const` up under the axiom's mangled id.

## Generated Interfaces {#plugin_codegen}

Every plugin's `<plugin>.mim` file is also machine-readable input to `mim` itself:
the switches `--output-h`, `--output-py`, and `--output-md` turn it into, respectively, a C++ header, a Python `IntEnum` module, and a Markdown documentation page.
MimIR's custom CMake command [`add_mim_plugin`](@ref add_mim_plugin_cmake) runs the freshly built `mim` binary over it once, in `--bootstrap` mode (which makes `plugin` directives behave as plain `import`s instead of `dlopen`ing other plugins that may not exist yet at header-generation time), asking for all three outputs in one invocation:

```sh
mim demo.mim --bootstrap \
    --output-h  build/include/mim/plug/demo/autogen.h \
    --output-py build/lib/mim/demo.py \
    --output-md build/docs/plug/demo.md
```

This happens automatically as part of the normal build — plugin authors never invoke this by hand.
See the [CLI reference](@ref cli) for the flags themselves.

### Generated Header {#plugin_h}

The switch `--output-h` walks the plugin's annexes after name binding and writes `include/mim/plug/<plugin>/autogen.h` into the build tree.
For `demo`, that's:

\include "include/mim/plug/demo/autogen.h"

For each annex tag it emits an `enum class <tag> : flags_t` (populated with subtags/aliases when the axiom has any — empty here, since `const_idx` has none), a forward declaration of the tag's normalizer matching [`mim::NormalizeFn`](@ref mim::NormalizeFn)'s `(type, callee, arg) -> const Def*` signature, and — once per plugin — a `register_normalizers` declaration plus the `MIM_<plugin>_NORMALIZER_IMPL` macro that defines it, wiring every axiom's normalizer into the `Normalizers` map under its mangled `Annex::Base<Tag>` id.
The `Annex::Base`/`Annex::Num` template specializations outside the plugin's namespace are implementation plumbing and are hidden from Doxygen via `#ifndef DOXYGEN`.

This generated header is never checked in and never included directly.
Instead, each plugin checks in a thin wrapper at `include/mim/plug/<plugin>/<plugin>.h` that plugin sources `#include` — [`demo.h`](@ref demo_h), shown above, is exactly that wrapper.

Larger plugins (e.g. [core](@ref core), [mem](@ref mem), [clos](@ref clos)) follow the same wrapper pattern but add hand-written declarations around the `#include`, so plugin authors can extend the generated boilerplate without editing generated code.
`add_mim_plugin`'s `INSTALL` option installs `autogen.h` itself (not the wrapper) into `<prefix>/include/mim/plug/<plugin>/` for out-of-tree consumers.

### Generated Python Module

The switch `--output-py` emits the same information as an `IntEnum`, for tooling written in Python:

\include "demo.py"

A tag with subtags gets its own `_<plugin>_<tag>(IntEnum)` class (later aliased as `<plugin>.<tag>`) instead of a plain member.
This is the `mim.plug.<plugin>` module the Python bindings re-export; see [Loading Plugins](@ref pyplugins) in the [Python Bindings](@ref python) guide for `world.annex(...)`/`world.call(...)` from Python.

### Generated Markdown Page

The switch `--output-md` works differently from `-h`/`-py`: rather than being derived from the bound AST, it is streamed straight out of the lexer while it scans `<plugin>.mim`.
Text inside `///` doc comments (ordinary Markdown, including Doxygen commands like `[TOC]` and `@see`) is copied through verbatim; every stretch of actual Mim syntax between comments is wrapped in a fenced ` ``` ` code block.
The result for `demo.mim` is written to `docs/plug/demo.md` in the build tree:

\include "plug/demo.md"

`docs/Doxyfile.in`'s `INPUT` includes that build-tree `docs/plug` directory, so Doxygen picks up every plugin's generated `.md` file directly as a documentation page, anchored at whatever `{#...}` id the plugin's leading doc comment declares (`{#demo}` here — the same anchor used by `@ref demo` elsewhere in the docs).
`add_mim_plugin` also adds a sidebar tab (of type `user`, pointing at `@ref <plugin>`) to the Doxygen layout for every registered plugin, so this page is linked from the sidebar automatically.
Writing good `///` doc comments in a plugin's `.mim` file is therefore both the axiom documentation and the plugin's Doxygen page — there is no separate place to describe a plugin's annexes.

## Plugin Registry

The [MimIR Plugin Registry](https://mimir.github.io/plugins) is the central hub for discovering, sharing, and maintaining third-party MimIR plugins.
The registry lists available plugins and provides guidance on how to discover and use them.
If you've created a plugin you'd like to share with the community, please consider submitting it to the registry.

## Create a New In-Tree Plugin

Create a new in-tree plugin `foobar` based on the [demo](@ref demo) plugin:

```sh
./scripts/new_plugin.py foobar
```

The script also supports `-h`/`--help` and prints the same usage text when called incorrectly.

By default, the script creates an in-tree plugin and updates `src/mim/plug/CMakeLists.txt`.
The generated files are:

- `src/mim/plug/<plugin>/<plugin>.mim`
- `src/mim/plug/<plugin>/CMakeLists.txt`
- `src/mim/plug/<plugin>/<plugin>.cpp`
- `src/mim/plug/<plugin>/normalizers.cpp`
- `include/mim/plug/<plugin>/<plugin>.h`
- `lit/<plugin>/const.mim`

## Create a Third-Party Plugin

To create a self-contained third-party plugin repository in `extra/`, use:

```sh
./scripts/new_plugin.py foobar --extra
```

This creates `extra/<plugin>/` with:

- `<plugin>.mim`
- `CMakeLists.txt`
- `src/<plugin>.cpp`
- `src/normalizers.cpp`
- `include/mim/plug/<plugin>/<plugin>.h`
- `lit/const.mim`
- `.github/workflows/{linux,macos,windows}.yml` — GitHub Actions CI/CD workflows

In `--extra` mode, the script also:
- Initializes a new Git repository for the plugin
- Generates GitHub Actions workflows that automatically build and test the plugin against the main MimIR repository
- Patches workflow configurations to clone the main repository as the parent and the plugin as a submodule in `extra/<plugin>`

### Third-Party Plugin Discovery {#extra_plugins}

@see [`extra/README.md`](https://github.com/mimir/mimir/blob/master/extra/README.md)

If you clone a plugin repository into `extra/`, MimIR picks it up automatically during configuration when the repository contains a `CMakeLists.txt` as a direct child of `extra/`.

- If it also contains `lit/*.mim` tests, they are picked up automatically by the main `lit` target as well.
- If it also contains `test/*.cpp` unit tests, they are built as `mim-<plugin>-test` and picked up automatically by `ctest` and the `test-all` target as well.

## Extract an Existing In-Tree Plugin

To move an existing in-tree plugin into `extra/foobar`, use:

```sh
./scripts/extract_plugin.py foobar
```

This moves:

- `src/mim/plug/<plugin>/` into `extra/<plugin>/`
- `include/mim/plug/<plugin>/` into `extra/<plugin>/include/...`
- `lit/<plugin>/` into `extra/<plugin>/lit/`

It also:

- Rewrites the extracted `CMakeLists.txt` for out-of-tree use
- Removes the plugin from the in-tree plugin list so it is picked up through `extra/` instead
- Generates GitHub Actions workflows that automatically build and test the extracted plugin against the main MimIR repository

The extracted plugin is staged with `git add` but not committed, allowing you to review the changes before committing.

## Standalone Third-Party Builds

After installing MimIR, a third-party plugin only needs to find the `mim` package.
For example, a plugin called `foo` can be set up like this:

```cmake
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(foo)

if(NOT COMMAND add_mim_plugin)
    find_package(mim REQUIRED)
endif()

add_mim_plugin(foo
    SOURCES
        src/foo.cpp
        src/normalizers.cpp
)
```

Configure the project standalone with:

```sh
cmake .. -Dmim_DIR=<MIM_INSTALL_PREFIX>/lib/cmake/mim
```

The authoritative reference for `add_mim_plugin` itself lives in [`cmake/Mim.cmake`](@ref add_mim_plugin_cmake).

## Runtime Wrappers {#plugin_runtime}

Backends such as [`ll`](@ref ll) sometimes need to emit calls to functionality that is awkward or brittle to express as hand-written LLVM IR — for example libc helpers, or complex argument setup for vendor APIs.
Instead of emitting the implementation inline, a backend can offload it to a small C *wrapper* that is compiled to LLVM IR by `clang` and pulled into the output.
This keeps the backend focused on emitting LLVM and lets `clang` deal with platform- and version-specific details.

The [`add_mim_runtime`](@ref add_mim_runtime_cmake) CMake command compiles a plugin's C wrapper sources to textual LLVM IR:

```cmake
add_mim_plugin(foo
    SOURCES
        src/foo.cpp
)

add_mim_runtime(foo
    SOURCES
        rt/foo_rt.c
    INSTALL
)
```

All sources are merged into a single module `<libdir>/mim/rt/<plugin>_rt.ll` (next to the plugins) and, with `INSTALL`, installed alongside them — one runtime module per plugin, addressable by a well-known name no matter how many `.c` files it is split into.
This step is optional: it requires `clang` (discovered as `MIM_CLANG`; merging multiple sources additionally needs `llvm-link`) and is skipped when `clang` is unavailable or `MIM_BUILD_LL_RUNTIME` is `OFF`.

The [`ll`](@ref ll) backend locates such a runtime module via the driver's [search paths](@ref cli) and either embeds it into or links it with its emitted module, selected via `-X ll:rt=embed` (default) or `-X ll:rt=extern`; see the [CLI reference](@ref cli).
The in-tree examples are `src/mim/plug/ll/rt/mim_rt.c`, which provides `@mim_jmpbuf_size` for `%%clos.alloc_jmpbuf`, and `src/mim/plug/ll_nvptx/rt/mim_cuda_rt.c`, whose `@mim_cu_check` performs the `ll_nvptx` backend's CUDA driver-API error handling.
The `ll_nvptx` backend reuses the very same [`load_rt_module`](@ref mim::plug::ll::Emitter::load_rt_module) helper as `ll`, differing only in the runtime module it names.

The authoritative reference for `add_mim_runtime` lives in [`cmake/Mim.cmake`](@ref add_mim_runtime_cmake).

<div class="section_buttons">

| Previous |     Next |
|:---------|---------:|
| [Contributing \& Debugging](@ref coding) | [Developer Guide](@ref dev) |

</div>
