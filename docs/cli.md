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

Mim keeps three separate lookups, because the artifacts they find are different in kind:
a plugin library is host-native code, a `.mim` is portable source, and a backend runtime belongs to the target.

Two kinds of entry feed them.
A *plain directory* is probed as-is and is what `-P` / `-I` and their environment variables add.
A *prefix root* stands for an install tree and derives `<root>/lib/mim`, `<root>/share/mim`, and `<root>/lib/mim/rt` from itself;
`--prefix-path` / `MIM_PREFIX_PATH` add one, as do the install prefix and the tree `libmim` was loaded from.

| Looking for | Order |
| --- | --- |
| `libmim_<name>` | cwd, `-P`, `MIM_PLUGIN_PATH`, then each root's `lib/mim` |
| `<name>.mim` | cwd, `-I`, `MIM_IMPORT_PATH`, `-P`, `MIM_PLUGIN_PATH`, then each root's `share/mim` and `lib/mim` |
| runtime modules | cwd `rt`, `-P` and `MIM_PLUGIN_PATH` each with `rt`, then each root's `lib/mim/rt` |

Plugin directories are searched for imports too, since a plugin ships both of its halves together.
`mim -l` prints all three lists fully resolved.

A `plugin <name>;` directive is special: its `<name>.mim` is taken from the directory `libmim_<name>` was actually loaded from,
so the two halves of a plugin can never be paired up across different directories.
A bare `import <name>;` has no such anchor and resolves by the table above,
so spell an import of your own file as `import "<name>.mim"` if the name could collide with an installed plugin.

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

### Environment Variables {#clipluginenv}

A plugin may also read environment variables - typically to locate an external toolchain it shells out to.
It declares them as [`mim::PluginEnv`](@ref mim::PluginEnv)s next to the code that reads them, so the *Plugin Environment Variables* tables under [Usage](@ref cliusage) are generated from those declarations, just like the `-X` tables above.
Since a plugin only announces them once it is loaded, `mim -p <plugin> --help` lists the ones belonging to `<plugin>`.

<div class="section_buttons">

| Previous                      |                                   Next |
| :---------------------------- | -------------------------------------: |
| [A Tour of MimIR](@ref mimir) | [Mim Language Reference](@ref langref) |

</div>
