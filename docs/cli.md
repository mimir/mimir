# Command-Line Reference {#cli}

[TOC]

## Usage

\include "cli-help.sh"

In addition, you can specify more search paths with `-P` / `--plugin-path` and via the environment variable `MIM_PLUGIN_PATH`.
Mim looks for plugins in this order:

1. The current working directory.
2. All paths specified via `-P` / `--plugin-path` (in the given order).
3. All paths specified in the environment variable `MIM_PLUGIN_PATH` (in the given order).
4. `path/to/mim.exe/../../lib/mim`
5. `CMAKE_INSTALL_PREFIX/lib/mim`

## Passing Arguments to Plugins & Phases {#clipluginargs}

Plugins - and in particular backends - often need to be configured from the command line.
For example, a backend that invokes an external tool may want to forward optimization levels, a target triple for cross-compilation, or library paths.
Use `-X` / `--plugin-arg` for this:

```
mim foo.mim -p ll -X ll:o=out.ll -X compile:aggr=on
```

The syntax is `-X <plugin>:<arg>`:

- The option is repeatable; each occurrence contributes one argument.
- Only the *first* `:` separates `<plugin>` from `<arg>`, so `<arg>` may itself contain `:` or `=` (e.g. Windows paths or `key=value` pairs).
- Arguments are keyed by plugin name and collected on the [`mim::Driver`](@ref mim::Driver).
  A [`mim::Phase`](@ref mim::Phase) reads the arguments addressed to its own plugin via [`mim::Phase::args`](@ref mim::Phase::args); the interpretation of each `<arg>` is up to the plugin.

### Known Arguments

| Plugin              | Argument      | Effect                                                                                                                   |
|---------------------|---------------|--------------------------------------------------------------------------------------------------------------------------|
| [compile](@ref compile) | `aggr=<bool>` | Value of `%%compile.aggr`; toggles fixed-point iteration of the `opt` pipeline's `optimize` stage (default off). `<bool>` is `on`/`tt`/`true` or `off`/`ff`/`false`; a bare `aggr` means `on`. See @ref compile_cli_args. |
| [ll](@ref ll)           | `o=<file>`    | Write the LLVM IR to `<file>` instead of the default `<world>.ll`/`a.ll`. See @ref ll_cli_args.                          |
| [ll_nvptx](@ref ll_nvptx) | `o=<file>`         | Write the (host) LLVM IR to `<file>` instead of the default `<world>.ll`/`a.ll`. See @ref ll_nvptx_cli_args.                             |
| [ll_nvptx](@ref ll_nvptx) | `o-dev=<file>`     | Write the device LLVM IR to `<file>` instead of the default `<world>_dev.ll`/`a_dev.ll`. See @ref ll_nvptx_cli_args.                     |
| [ll_nvptx](@ref ll_nvptx) | `no-embed`         | Don't embed compiled device binary in host LLVM IR, which is the default behavior. See @ref ll_nvptx_cli_args.       |
| [ll_nvptx](@ref ll_nvptx) | `no-ptx-embed`     | If embedding device binary, don't embed PTX image into fat binary (default: PTX and CUBIN). See @ref ll_nvptx_cli_args.            |
| [ll_nvptx](@ref ll_nvptx) | `no-cubin-embed`   | If embedding device binary, don't embed CUBIN image into fat binary (default: PTX and CUBIN). See @ref ll_nvptx_cli_args.          |
| [ll_nvptx](@ref ll_nvptx) | `sm=<SM>`          | If embedding device binary, compile the device binary for compute capability sm_<SM>. See @ref ll_nvptx_cli_args.                  |
| [ll_nvptx](@ref ll_nvptx) | `libdevice=<path>` | If embedding device binary and if linking libdevice, link against the libdevice NVVM library at <path> instead of trying to find it via CUDA paths. See @ref ll_nvptx_cli_args. |
| [ll_nvptx](@ref ll_nvptx) | `X<tool>=<args>`   | If embedding device binary invoke <tool> with <args>. See @ref ll_nvptx_cli_args for more details.     |

## Debugging Features {#clidebug}

- The breakpoint-oriented flags below are developer options that are only available when MimIR is built with `MIM_ENABLE_CHECKS`.
- You can increase the log level with `-V`.
  - No `-V` corresponds to mim::Log::Level::Error.
  - `-V` corresponds to mim::Log::Level::Warn.
  - `-VV` corresponds to mim::Log::Level::Info.
  - `-VVV` corresponds to mim::Log::Level::Verbose.
  - `-VVVV` corresponds to mim::Log::Level::Debug. This output only exists in a Debug build of MimIR.
  - `-VVVVV` corresponds to mim::Log::Level::Trace. This output only exists in a Debug build of MimIR.
- You can trigger a breakpoint when constructing a [`mim::Def`](@ref mim::Def) with a specific global id.

  For example, this triggers a breakpoint when the [`mim::Def`](@ref mim::Def) with [`gid`](@ref mim::Def::gid) `4223` is created:

  ```
  mim -b 4223 in.mim
  ```

- You can also trigger breakpoints at other specific events, for example when an alpha-equivalence check fails via `--break-on-alpha`.
- You can measure per-[`mim::Phase`](@ref mim::Phase) wall-clock time with `--profile` and `--output-profile`.
  See [Profiling](@ref profiling) for details, including how to view `--profile trace` output in `chrome://tracing`.
