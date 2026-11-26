# Playground

The playground runs an emscripten build of `mim` client-side; `mim.wasm` ships as a release asset, not in the Pages repo, which would bloat with every master push.

- Configure the wasm tree with `emcmake cmake -DMIM_NATIVE_MIM=<native build>/bin/mim` and `-fwasm-exceptions` in both compiler and linker flags; emscripten forces `MIM_STATIC_PLUGINS`.
- `mim` runs in a Web Worker (`mim-worker.js`), since `terminate()` is the only way to stop a wasm loop; the page arms a watchdog and spawns a fresh worker after a kill.
- V8 gives wasm frames about 1MB of engine stack that `-sSTACK_SIZE` does not control, so deep recursion is the wasm hazard.
- When sweeping lit through the wasm build, stage the whole directory into MEMFS, or every sibling `import` fails.
- `math/lit_float_fp16.mim` is the one expected difference to native: emscripten has no `__STDCPP_FLOAT16_T__`.

Testing headlessly needs no puppeteer: serve `build-wasm/playground` with `python3 -m http.server`, launch `chromium --headless --no-sandbox --disable-popup-blocking --remote-debugging-port=9333 --user-data-dir=<tmp>`, and drive it from node, whose global `WebSocket` speaks CDP (`/json/list` lists the targets, each pop-out is its own target).
