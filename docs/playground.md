# Playground {#playground}

[TOC]

The playground is MimIR in the browser: the `mim` CLI compiled to WebAssembly by [Emscripten](https://emscripten.org/), plus a small page that feeds it a buffer and renders what comes back.
Its sources live in `web/`; a build stages them next to the Emscripten artifacts in `${CMAKE_BINARY_DIR}/playground`, so serving that directory over HTTP is all it takes to run it.
A build of the current `master` is live at <https://mimir.github.io/playground/>.

| File                 | Role                                                                                              |
| -------------------- | ------------------------------------------------------------------------------------------------- |
| `index.html`         | The page: editor on the left, tabbed output on the right.                                         |
| `graph.html`         | The pop-out window: the graph, and nothing else.                                                  |
| `playground.css`     | Styling, including the light/dark palette and the log's terminal colors.                          |
| `playground.js`      | Editor, tabs, run loop, Graphviz layout.                                                          |
| `graphview.js`       | Pan, zoom and highlight over a laid-out graph; the **Graph** tab and the pop-out share one.        |
| `popout.js`          | The pop-out's glue: it renders the SVG the page sends over.                                       |
| `mim-worker.js`      | Runs `mim` in a Worker, so a program that never terminates can be killed.                         |
| `code.js`            | `docs/code.js`, staged; the lexer machinery the page and the docs share.                          |
| `mim-code.js`        | `docs/mim.js`, staged; the Mim lexer, renamed out of Emscripten's `mim.js` way.                   |
| `llvm-code.js`       | `docs/llvm.js`, staged; the LLVM lexer of the **LLVM** tab.                                       |
| `darkmode-toggle.js` | `doxygen-awesome-darkmode-toggle.js`, staged; the very toggle these docs use.                     |
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
The **Mim** tab's _typed lets_ box is `--mim-typed-let` and its _local blocks_ box is `--mim-local`; see @ref cli for what each one does.
_ASCII_ is `-a`, which is a global flag rather than a Mim-only one: it swaps the UTF-8 spellings out of every output, the graph's labels included.

The _vi_ box in the editor bar switches the editor to vi keybindings — [@replit/codemirror-vim](https://github.com/replit/codemirror-vim), fetched from a CDN the first time you tick it.
The bar below the editor shows the mode and takes `:` commands; `:w` runs the program, which is the only thing there is to write to.
The box is remembered across reloads, and it is gone altogether when the editor falls back to a plain textarea because CodeMirror did not load.

A `?src=` query parameter loads code instead of the first example, so a link can carry a whole program — [this one](https://mimir.github.io/playground/?src=plugin%20core%3B%0Ause%20core.ops.u.w%3B%0A%0Aextern%20fun%20inc%20(x%3A%20I32)%3A%20I32%20%3D%20return%20(x%20%2B%201I32)%3B%0A) increments an `I32`.
Percent-encode it — `encodeURIComponent` in the browser's console produces exactly what the page expects; a `+` stands for itself and is *not* a space.
The picker keeps such a program under _(custom)_, so loading an example does not lose it.
Every Mim snippet in these docs is a link of that kind: hover it and the ▶ button next to the copy button opens it here.

The **Graph** tab lays out `--output-dot` with [Graphviz](https://graphviz.org/), and its checkboxes are exactly the CLI's `--dot-*` switches:

| Checkbox       | Flag                   |
| -------------- | ---------------------- |
| default filter | `--dot-default-filter` |
| type edges     | `--dot-follow-types`   |
| inline consts  | `--dot-inline-consts`  |
| hidden edges   | `--dot-show-hidden`    |
| no tooltips    | `--dot-no-tooltip`     |
| lean labels    | `--dot-lean-labels`    |

See @ref cli for what each one does.
Since they are compile-time flags, toggling one re-runs the compiler.
`--dot-all-annexes` has no checkbox: it buries any program of this size in the annexes it pulls in.

Layout happens on the page, so a graph beyond 512 KB of DOT is refused rather than attempted.
The last two checkboxes are the ones that buy room — on an optimized `ackermann.mim` the tooltips are two fifths of the file and the labels another third, and the two together take it from 30 KB to 7 KB.
They are what a refused graph needs; the price is the hover text and the ports an edge docks at.

### Navigating

Drag to pan and turn the wheel to zoom at the pointer; **Fit**, a double-click, or `0` frames the whole graph again, and `+`/`-` zoom from the keyboard.
A re-run keeps the current view: the editor recompiles on every pause in typing, so re-framing each time would leave no way to stay zoomed in on one spot.

Hovering a node dims everything but that node, its direct neighbours and the edges between them; a click pins that focus, and a click elsewhere or `Esc` releases it.
The highlight paints detached edges too, so it shows per node what _hidden edges_ shows for the whole graph — without a recompile.

The button left of the zoom controls opens the graph in a window of its own, with the same controls and nothing else.
The page lays the DOT out and sends the SVG over, so the pop-out needs neither Graphviz nor the compiler and follows every re-run — including while the **Graph** tab itself is down, which leaves the whole page for the editor and the other tabs.

### Dark mode

The toggle in the header is the [doxygen-awesome](https://github.com/jothepro/doxygen-awesome-css) one these docs use, staged from the submodule, so both offer the same control and — served from one origin — share the stored preference.
The graph's colors are baked by the compiler rather than applied by CSS, so flipping the theme re-runs it with `--dot-dark`, which drops the white backdrop and lightens the edges.

## CI

`.github/workflows/playground.yml` does all of the above on every push, compiles a program with the binary it just built — a wasm-only regression such as the one behind `Def::ops_ptr` fails there instead of silently shipping — and uploads `build-wasm/playground/` as an artifact.

A push to `master` also deploys that directory into `playground/` of the [site repository](https://github.com/mimir/mimir.github.io), next to the Doxygen output, via `docs/deploy.py --subdir playground`.
The docs deploy sweeps the site root, so `playground/` only survives by being listed in that script's `FOREIGN_DIRS`.
Both workflows push to the same repository from their own concurrency group and rely on the script's rebase retry; they never write the same paths.
