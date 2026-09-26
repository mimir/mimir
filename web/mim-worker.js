// mim runs here so that a program looping forever cannot take the UI with it.
importScripts('mim.js');

let compiled = null; // compiling the 2.4MB wasm once makes every later run cost milliseconds
let next = null;     // booted ahead of time, so a run waits for the compiler, not its startup

// A fresh instance per run keeps each compile in a pristine World.
function instantiate(log) {
    return createMim({
        print: log,
        printErr: log,
        noExitRuntime: true,
        locateFile: file => new URL(file, self.location.href).href,
        instantiateWasm: (imports, receive) => {
            if (compiled)
                WebAssembly.instantiate(compiled, imports).then(instance => receive(instance, compiled));
            else
                WebAssembly.instantiateStreaming(fetch('mim.wasm'), imports)
                    .then(({ module, instance }) => receive(instance, compiled = module));
            return {};
        },
    });
}

// The log belongs to the run that claims the instance, not to the one that booted it.
function boot() {
    const log = [];
    const module = instantiate(s => log.push(s));
    module.catch(() => {}); // nobody awaits the boot until a run claims it
    return { log, module };
}

function read(M, path) {
    try { return M.FS.readFile(path, { encoding: 'utf8' }); } catch { return null; }
}

function diagnose(e) {
    const msg = String(e?.message ?? e).replace(/\x1b\[[0-9;]*m/g, '');
    if (/call stack|too much recursion/i.test(msg)) return 'the compiler recursed deeper than the browser stack allows';
    if (/memory|allocation failed/i.test(msg)) return 'the compiler ran out of memory';
    return msg;
}

self.onmessage = async ({ data }) => {
    const pending = next ?? boot();
    next = null;
    const log = pending.log;

    try {
        const M = await pending.module;
        self.postMessage({ ready: true }); // the page's run clock starts here
        M.FS.writeFile('/in.mim', data.src);
        const code = M.callMain(data.args);
        const out = {};
        for (const kind of ['mim', 'ast', 'll', 'dot', 'nest']) out[kind] = read(M, `/out.${kind}`);
        self.postMessage({ code, log, out });
    } catch (e) {
        self.postMessage({ code: -1, log, error: diagnose(e) });
    }

    next = boot();
};
