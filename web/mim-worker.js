// mim runs here, not on the page: a program that loops forever or traps must not take the UI with it.
importScripts('mim.js');

let compiled = null; // compiling the 2.4MB wasm once makes every later run cost milliseconds

// A fresh instance per run keeps each compile in a pristine World.
function instantiate(log) {
    return createMim({
        print: log,
        printErr: log,
        noExitRuntime: true,
        locateFile: file => new URL(file, self.location.href).href,
        instantiateWasm: (imports, receive) => {
            const done = (module, instance) => {
                compiled = module;
                receive(instance, module);
            };
            if (compiled)
                WebAssembly.instantiate(compiled, imports).then(instance => done(compiled, instance));
            else
                WebAssembly.instantiateStreaming(fetch('mim.wasm'), imports)
                    .then(({ module, instance }) => done(module, instance));
            return {};
        },
    });
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
    const log = [];
    let M;

    try {
        M = await instantiate(s => log.push(s));
    } catch (e) {
        self.postMessage({ code: -1, log, error: diagnose(e) });
        return;
    }

    self.postMessage({ ready: true }); // the page's run clock starts here, not at page load

    try {
        M.FS.writeFile('/in.mim', data.src);
        const code = M.callMain(data.args);
        const out = { mim: read(M, '/out.mim'), ll: read(M, '/out.ll'), dot: read(M, '/out.dot') };
        self.postMessage({ code, log, out });
    } catch (e) {
        self.postMessage({ code: -1, log, error: diagnose(e) });
    }
};
