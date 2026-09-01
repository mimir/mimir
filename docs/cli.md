# Command-Line Reference {#cli}

[TOC]

## Usage

\include{doc} "cli-help.md"

## Remarks

@note The *Developer Options* only exist if MimIR was built with `MIM_ENABLE_CHECKS`; see the [CMake switches](@ref building).

In addition, you can specify more search paths with `-P` / `--plugin-path` and via the environment variable `MIM_PLUGIN_PATH`.
Mim looks for plugins in this order:

1. The current working directory.
2. All paths specified via `-P` / `--plugin-path` (in the given order).
3. All paths specified in the environment variable `MIM_PLUGIN_PATH` (in the given order).
4. `path/to/mim.exe/../../lib/mim`
5. `CMAKE_INSTALL_PREFIX/lib/mim`

## Generating Plugin Interfaces {#cligen}

The switches `--output-h`, `--output-py`, and `--output-md` turn a `<plugin>.mim` file into, respectively, a C++ header, a Python `IntEnum` module, and a Markdown documentation page.
The flag `--bootstrap` makes `plugin` directives behave as plain `import`s instead of `dlopen`ing other plugins, which matters when generating these outputs during the build, before the plugin's own shared library even exists.
The CMake command [`add_mim_plugin`](@ref add_mim_plugin_cmake) runs all three automatically for every plugin as part of the normal build; see [Generated Interfaces](@ref plugin_codegen) for a full walkthrough using the [demo](@ref demo) plugin.

## Passing Arguments to Plugins {#clipluginargs}

Plugins - and in particular backends - often need to be configured from the command line.
For example, a backend that invokes an external tool may want to forward optimization levels, a target triple for cross-compilation, or library paths.
Use `-X` / `--plugin-arg` for this:

```
mim foo.mim -p ll -X ll:o=out.ll -X compile:aggr=on
```

The syntax is `-X <plugin>:<arg>`:

- The option is repeatable; each occurrence contributes one argument.
- Only the _first_ `:` separates `<plugin>` from `<arg>`, so `<arg>` may itself contain `:` or `=` (e.g. Windows paths or `key=value` pairs).
- Arguments are keyed by plugin name and collected on the [`mim::Driver`](@ref mim::Driver).
  A [`mim::Phase`](@ref mim::Phase) reads the arguments addressed to its own plugin via [`mim::Phase::args`](@ref mim::Phase::args); the interpretation of each `<arg>` is up to the plugin.

### Known Arguments

| Plugin                    | Argument                                | Effect                                                                                                                                                                                         |
| ------------------------- | --------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [compile](@ref compile)   | `aggr=<bool>`                           | Value of `%%compile.aggr`; toggles fixed-point iteration of the `opt` pipeline's `optimize` stage (default off). `<bool>` is `on`/`tt`/`true` or `off`/`ff`/`false`; a bare `aggr` means `on`. |
| [ll](@ref ll)             | `o=<file>` <br> `output=<file>`         | Write the LLVM IR to `<file>` instead of the default `<world>.ll`/`a.ll`.                                                                                                                      |
| [ll](@ref ll)             | `rt=embed` <br> `rt=extern`             | How the C [runtime wrappers](@ref plugin_runtime) reach the output: `embed` (default) splices their LLVM IR into the emitted module (self-contained); `extern` only `declare`s them so the runtime is linked in separately.  |
| [ll_nvptx](@ref ll_nvptx) | `o=<file>` <br> `output=<file>`         | Write the (host) LLVM IR to `<file>` instead of the default `<world>.ll`/`a.ll`.                                                                                                               |
| [ll_nvptx](@ref ll_nvptx) | `o-dev=<file>` <br> `output-dev=<file>` | Write the device LLVM IR to `<file>` instead of the default `<world>_dev.ll`/`a_dev.ll`.                                                                                                       |
| [ll_nvptx](@ref ll_nvptx) | `rt=embed` <br> `rt=extern`             | Like `ll`'s `rt`, but for the host module's C [runtime wrappers](@ref plugin_runtime) (e.g. `@mim_cu_check`): `embed` (default) splices their LLVM IR in; `extern` only `declare`s them.       |
| [ll_nvptx](@ref ll_nvptx) | `no-embed`                              | Don't embed the compiled device binary into the host LLVM IR (embedding is the default on Linux).                                                                                              |
| [ll_nvptx](@ref ll_nvptx) | `no-ptx-embed`                          | If embedding the device binary, don't embed the PTX image into the fat binary (default: embed both PTX and CUBIN).                                                                             |
| [ll_nvptx](@ref ll_nvptx) | `no-cubin-embed`                        | If embedding the device binary, don't embed the CUBIN image into the fat binary (default: embed both PTX and CUBIN).                                                                           |
| [ll_nvptx](@ref ll_nvptx) | `sm=<SM>`                               | If embedding device binary, compile the device binary for compute capability sm\_`<SM>`.                                                                                                       |
| [ll_nvptx](@ref ll_nvptx) | `libdevice=<path>`                      | If embedding device binary and if linking libdevice, link against the libdevice NVVM library at `<path>` instead of trying to find it via CUDA paths.                                          |
| [ll_nvptx](@ref ll_nvptx) | `X<tool>=<args>`                        | If embedding device binary invoke `<tool>` with `<args>`.                                                                                                                                      |

## Diagnostics {#clidiag}

Errors and warnings are reported as `<file>:<row>:<col>: error: <message>`, followed by the offending source line with a caret underneath and any notes indented below it.
Pass `--no-snippet` to omit the source line and caret, e.g. when the output is consumed by a script.
Use `--loc-style` to pick how much of a location that header spells out:

| `<style>` | Renders as               |
| --------- | ------------------------ |
| `full`    | `path:row:col-row:col` (default) |
| `rowcol`  | `path:row:col`           |
| `row`     | `path:row`               |
| `msvc`    | `path(row,col)`          |

Color is decided by the terminal and honors `NO_COLOR`, `CLICOLOR=0`, and `CLICOLOR_FORCE`.
