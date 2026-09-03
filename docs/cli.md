# Command-Line Reference {#cli}

[TOC]

## Usage {#cliusage}

\include{doc} "cli-help.md"

@note The _Developer Options_ only exist if MimIR was built with `MIM_ENABLE_CHECKS`; see the [CMake switches](@ref building).

## Diagnostics {#clidiag}

Errors and warnings are reported as `<file>:<row>:<col>: error: <message>`, followed by the offending source line with a caret underneath and any notes indented below it.
Pass `--no-snippet` to omit the source line and caret, e.g. when the output is consumed by a script.
Use `--loc-style` to pick how much of a location that header spells out:

| `<style>` | Renders as                       |
| --------- | -------------------------------- |
| `full`    | `path:row:col-row:col` (default) |
| `rowcol`  | `path:row:col`                   |
| `row`     | `path:row`                       |
| `msvc`    | `path(row,col)`                  |

## Plugins

### Search Paths

Mim looks for plugins in this order:

1. The current working directory.
2. All paths specified via `-P` / `--plugin-path` (in the given order).
3. All paths specified in the environment variable `MIM_PLUGIN_PATH` (in the given order).
4. `path/to/mim.exe/../../lib/mim`
5. `CMAKE_INSTALL_PREFIX/lib/mim`

### Arguments {#clipluginargs}

Plugins - and in particular backends - often need to be configured from the command line.
For example, a backend that invokes an external tool may want to forward optimization levels, a target triple for cross-compilation, or library paths.
Use `-X` / `--plugin-arg` for this:

```
mim foo.mim -p ll -X ll:o=out.ll -X compile:aggr
```

The syntax is `-X <plugin>:<arg>`:

- The option is repeatable; each occurrence contributes one argument.
- Only the _first_ `:` separates `<plugin>` from `<arg>`, so `<arg>` may itself contain `:` or `=` (e.g. Windows paths or `key=value` pairs).
- Arguments are keyed by plugin name and collected on the [`mim::Driver`](@ref mim::Driver).
  A [`mim::Phase`](@ref mim::Phase) reads the arguments addressed to its own plugin via [`mim::Phase::args`](@ref mim::Phase::args); the interpretation of each `<arg>` is up to the plugin.

Each plugin declares the arguments it understands right next to the code that reads them, so the `-X <plugin>:<arg>` tables under [Usage](@ref cliusage) are generated from those declarations.

<div class="section_buttons">

| Previous                      |                                   Next |
| :---------------------------- | -------------------------------------: |
| [A Tour of MimIR](@ref mimir) | [Mim Language Reference](@ref langref) |

</div>
