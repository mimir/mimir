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
