# Python Bindings {#python}

[TOC]

MimIR ships a Python package called `mim`.
Its binding code is generated directly from the C++ headers via [nanobind](https://nanobind.readthedocs.io/) (see [Extending the Bindings](#python_extending)), so it mirrors a large portion of the C++ API rather than a hand-picked subset, with a small Python layer on top for convenience.
Because the surface is generated, it stays close to the C++ API by construction.
Names such as `lit_i8`, `type_i32`, `arr`, `mut_con`, and `optimize` are exposed with the same spelling you see in the C++ code.
You have to enable `MIM_BUILD_PYTHON` (default) during configuration for Python support.
The build creates a virtual environment at `build/.venv` and installs `mim` into it.
Only the initial creation of that venv needs a network connection to fetch `pip`, `setuptools`, `wheel`, and `pytest`.
Once `build/.venv` exists, all further builds stay offline.

Activate that environment before importing `mim`:

```sh
source build/.venv/bin/activate
python -c 'import mim; print(mim.Driver)'
```

Run the Python test suite with:

```sh
cmake --build build --target test-py
```

## Package Layout

The public entry point is:

```python
import mim
```

Internally, the package consists of a few layers:

- `_mim`: the compiled nanobind extension module (its C++ sources are generated at build time, see [Extending the Bindings](#python_extending)),
- `mim`: the public Python package that re-exports `_mim` and adds helper functions,
- `mim.plug.<name>`: generated plugin enum facades, for example `mim.plug.core`,

The package also ships `.pyi` stubs, so editors and type checkers can see the exported binding surface.

## First Steps

The usual entry point is [`Driver`](@ref mim::Driver), just like in C++:

```python
import mim

driver = mim.Driver()
world = driver.world()

i8 = world.type_i8()
lit = world.lit_i8(7)

assert isinstance(lit, mim.Def)
assert lit.world() is world
```

`Def` objects are lightweight handles into the current [`World`](@ref mim::World).
The bindings currently expose a few frequently used helpers:

```python
m = world.mut_con([world.type_bool(), world.type_i8()]).set("pair")
var = m.var()

first = var.proj(0)
again = var[0]
typed = var[2, 0]
parts = var.projs(2)
```

So the Python surface follows the same “named IR handle” model as the C++ API rather than copying nodes into Python-owned objects.

## Loading Plugins

Runtime plugins are still discovered the same way as in C++.
`Driver()` usually picks up the in-tree plugin build directory automatically, including from the editable Python package.
Use `add_search_path(...)` only when you want to load plugins from an extra directory:

```python
import mim

driver = mim.Driver()
driver.load_plugins(["core"])
world = driver.world()
```

The package also stages generated Python enums for in-tree plugins.
Those are re-exported as `mim.plug.<name>` modules:

```python
import mim.plug.core as core

bit_and = world.annex(core.bit2.and_)
```

For plugin calls, the convenience helper `World.call(...)` is the nicest entry point:
It is designed around these generated plugin enums and folds each Python argument or argument list into `implicit_app` calls:

```python
import mim.plug.regex as regex

driver.load_plugins(["core", "compile", "regex", "opt"])
expr = world.call(regex.any)
```

If you need finer control, call `world.annex(...)`, `world.implicit_app(...)`, or `world.app(...)` directly.

## Error Handling Helpers

The top-level Python package adds a few small utilities for MimIR exceptions:

```python
import io
import mim

buffer = io.StringIO()

with mim.guard_mim_errors(file=buffer):
    raise mim.MIM_Error("boom")
```

Available helpers:

- `print_mim_error(exc, file=...)`
- `guard_mim_errors(...)`
- `catch_mim_errors(...)`

These are ordinary Python wrappers around the `MIM_Error` exception type exported by the binding module.

## Coverage

Because the bindings are generated straight from the C++ headers (see [Extending the Bindings](#python_extending)), the Python surface tracks the C++ API broadly rather than being a hand-picked subset.
Core types such as [`Def`](@ref mim::Def), [`World`](@ref mim::World), [`Driver`](@ref mim::Driver), and [`Lam`](@ref mim::Lam) and their public methods are exposed automatically, with the same spelling.

Typical workflows are all supported out of the box:

- constructing IR fragments directly from Python,
- loading plugins and calling generated annex enums,
- driving optimization over a [`World`](@ref mim::World) (`world.optimize()`), and
- end-to-end regex/JIT experiments (which emit LLVM IR and invoke `clang` under the hood).

The only gaps are the handful of constructs libclang cannot express as a plain binding (e.g. template methods), and closing them is trivial — expose the C++ method, or add a small `.nbextra` patch, as described next.

## Extending the Bindings {#python_extending}

Most of the binding code is **generated at build time** rather than written by hand.
`scripts/gen_nanobind_bindings.py` parses the C++ headers with libclang and emits one nanobind translation unit per header into `build/py/auto_bindings/`.
The set of headers to wrap and the order in which their `init_*` functions are registered are listed in `py/CMakeLists.txt`; the module entry point (`NB_MODULE`) is generated from that same manifest, so it never drifts out of sync.

To expose more of an already-wrapped class, just add the C++ method: on the next build it is picked up automatically.
Adding a new header to the manifest in `py/CMakeLists.txt` wraps a new class.

Some declarations cannot be expressed by the generator — template methods, private/overloaded constructors, or signatures that need a Python-friendly conversion (e.g. accepting a list where the C++ takes `Defs`).
For these, drop a companion `<header-stem>.nbextra` file into `py/nbextra/` (it is picked up automatically when it exists).
An `.nbextra` file is a small patch applied to the generated unit, with bracketed sections:

- `[include]` — extra `#include` lines,
- `[class:Name]` — a `.def(...)` chain appended to that class's binding,
- `[skip:Name]` — whitespace-separated method names to drop from that class (pair with `[class:Name]` to substitute a hand-written binding),
- `[standalone]` — raw code injected after all class/enum blocks.

A `[class:Name]`/`[skip:Name]` that matches no parsed class fails the build loudly, so a stale name after a C++ rename cannot silently drop bindings.

A handful of units are still fully hand-written under `py/bindings/` (listed in `py/CMakeLists.txt`): `error.cpp` registers the `MIM_Error` exception, while `ast.cpp` and `parser.cpp` cover surface the generator does not yet handle.

## Embedded Python DSL

The file `mim.plug.regex` provides a higher-level wrapper around the regex plugin in form of an embedded domain-specific language.
It builds MimIR through overloaded operators, runs optimization, emits LLVM IR, invokes `clang`, and loads the resulting shared object via `ctypes`.

\include "examples/regex.py"

`RegBuilder` loads the required plugins by default, and `pattern.jit()` returns Python callables that wrap the exported compiled functions.
