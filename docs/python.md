# Python Bindings {#python}

[TOC]

MimIR ships a Python package called `mim`.
It wraps selected parts of the C++ API via [nanobind](https://nanobind.readthedocs.io/) and adds a small Python layer on top for convenience.
The package is intentionally close to the C++ API.
Names such as `lit_i8`, `type_i32`, `arr`, `mut_con`, and `optimize` are exposed with the same spelling you see in the C++ code.
You have to enable `MIM_BUILD_PYTHON` (default) during configuration for Python support.

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

- `_mim`: the compiled nanobind extension module,
- `mim`: the public Python package that re-exports `_mim` and adds helper functions,
- `mim.plug.<name>`: generated plugin enum facades, for example `mim.plug.core`,

The package also ships `.pyi` stubs, so editors and type checkers can see the exported binding surface.

## First Steps

The usual entry point is [Driver](@ref mim::Driver), just like in C++:

```python
import mim

driver = mim.Driver()
world = driver.world()

i8 = world.type_i8()
lit = world.lit_i8(7)

assert isinstance(lit, mim.Def)
assert lit.world() is world
```

`Def` objects are lightweight handles into the current [World](@ref mim::World).
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

## Current Scope

The Python bindings are still intentionally selective.
They already support useful workflows for:

- constructing small IR fragments directly from Python,
- loading plugins and calling generated annex enums,
- driving optimization and backend emission through [Driver](@ref mim::Driver), and
- end-to-end regex/JIT experiments.

But the Python API is not yet a one-to-one mirror of the full C++ surface.
If you need a feature that exists in C++ but not in Python yet, the binding files under `py/bindings/` are the place to extend.

## Embedded Python DSL

The file `mim.plug.regex` provides a higher-level wrapper around the regex plugin in form of an embedded domain-specific language.
It builds MimIR through overloaded operators, runs optimization, emits LLVM IR, invokes `clang`, and loads the resulting shared object via `ctypes`.

\include "examples/regex.py"

`RegBuilder` loads the required plugins by default, and `pattern.jit()` returns Python callables that wrap the exported compiled functions.
