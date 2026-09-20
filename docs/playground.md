# Playground {#playground}

[TOC]

The playground is MimIR in the browser: the `mim` CLI compiled to WebAssembly by [Emscripten](https://emscripten.org/), plus a small page that feeds it a buffer and renders what comes back.
Its sources live in `web/`; a build stages them next to the Emscripten artifacts in `${CMAKE_BINARY_DIR}/playground`, so serving that directory over HTTP is all it takes to run it.
A build of the current `master` is live at <https://mimir.github.io/playground/>.

| File                 | Role                                                                                              |
| -------------------- | ------------------------------------------------------------------------------------------------- |
| `index.html`         | The page: editor on the left, tabbed output on the right.                                         |
| `playground.css`     | Styling, including the light/dark palette and the log's terminal colors.                          |
| `playground.js`      | Editor, tabs, run loop, Graphviz layout.                                                          |
| `mim-worker.js`      | Runs `mim` in a Worker, so a program that never terminates can be killed.                         |
| `mim-code.js`        | `docs/mim.js`, staged; the page and the docs colour Mim with the very same lexer.                 |
| `examples/*.mim`     | `lit/docs/*.mim`, staged; the examples are the ones the lit suite covers.                         |
| `mim.{js,wasm,data}` | Emscripten's output; `mim.data` carries the plugins' `.mim` halves and the [ll](@ref ll) runtime. |

## Building

### Emscripten

Install the SDK once and put it in the environment of every shell that configures or builds:

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 6.0.9 # the version CI pins
~/emsdk/emsdk activate 6.0.9
source ~/emsdk/emsdk_env.sh
```

### A native `mim` first

A cross build cannot run the `mim` it just built, but bootstrapping a plugin needs one, so point `MIM_NATIVE_MIM` at a native binary — an ordinary `build/bin/mim` does.
Bring it up to date first:

```sh
cmake --build build -j16
```

@note Nothing checks that the binary matches the sources.
A `build/bin/mim` left over from an earlier state of the tree may abort on startup (`free(): invalid pointer`), and every plugin bootstrap fails with it.
Rebuild the native tree and try again.

### Configure and build

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm                     \
    -DCMAKE_BUILD_TYPE=Release                  \
    -DBUILD_SHARED_LIBS=OFF                     \
    -DBUILD_TESTING=OFF                         \
    -DMIM_BUILD_PYTHON=OFF                      \
    -DMIM_VERIFY_PLUGINS=OFF                    \
    -DMIM_NATIVE_MIM=$PWD/build/bin/mim
cmake --build build-wasm --target mim_playground -j16
```

`mim_playground` also stages the page, so a change to it alone is a second's rebuild — no need to touch the compiler.

## Running

The page loads its Worker and wasm over `fetch`, which `file://` refuses; serve the directory instead:

```sh
python3 -m http.server -d build-wasm/playground 8080
```

Then open <http://localhost:8080>.
The server keeps running until you stop it with `Ctrl-C`; after a rebuild just reload the page.

@note The browser caches `mim.wasm` aggressively.
If a freshly built compiler seems to behave like the old one, reload while bypassing the cache (`Ctrl-Shift-R`).

## Using it

The editor compiles on every keystroke, half a second after you stop typing; **Run** turns into **Stop** while a run is in flight and kills a program that does not terminate.
_optimize_ runs the [opt](@ref opt) pipeline and the [ll](@ref ll) backend; without it the program is only parsed and emitted again.

A `?src=` query parameter loads code instead of the first example, so a link can carry a whole program:
<https://mimir.github.io/playground/?src=extern%20lam%20id%20%7BT%3A%20*%7D%20(x%3A%20T)%3A%20T%20%3D%20x%3B>.
Percent-encode it — `encodeURIComponent` in the browser's console does exactly what the page expects; note that a `+` stands for itself and is *not* a space.
The picker keeps such a program under _(custom)_, so loading an example does not lose it.

The **Graph** tab lays out `--output-dot` with [Graphviz](https://graphviz.org/), and its checkboxes are exactly the CLI's `--dot-*` switches:

| Checkbox       | Flag                   |
| -------------- | ---------------------- |
| all annexes    | `--dot-all-annexes`    |
| default filter | `--dot-default-filter` |
| type edges     | `--dot-follow-types`   |
| inline consts  | `--dot-inline-consts`  |
| hidden edges   | `--dot-show-hidden`    |

See @ref cli for what each one does.
Since they are compile-time flags, toggling one re-runs the compiler.
Layout happens on the page, so a graph beyond 512 KB of DOT is refused rather than attempted — `--dot-all-annexes` reaches that on anything but a small program.

## CI

`.github/workflows/playground.yml` does all of the above on every push, compiles a program with the binary it just built — a wasm-only regression such as the one behind `Def::ops_ptr` fails there instead of silently shipping — and uploads `build-wasm/playground/` as an artifact.

A push to `master` also deploys that directory into `playground/` of the [site repository](https://github.com/mimir/mimir.github.io), next to the Doxygen output, via `docs/deploy.py --subdir playground`.
The docs deploy sweeps the site root, so `playground/` only survives by being listed in that script's `FOREIGN_DIRS`.
Both workflows push to the same repository from their own concurrency group and rely on the script's rebase retry; they never write the same paths.
