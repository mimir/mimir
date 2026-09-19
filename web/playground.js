import { Graphviz } from 'https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1/dist/index.js';

const EXAMPLES = {
    'sq.mim': `plugin core as *;

lam sq {T: *} (\`*: [T, T] → T) (x: T): T = x * x;

extern fun f(x: Nat): Nat =
    return (sq nat.mul x);
`,
    'count.mim': `plugin core;

use core.ops.u.w;

extern fun count(n: I32): I32 =
    loop (0I32, 0I32)
    where
        con loop (i acc: I32) = core.select (i < n, body, exit) ()
            where
                con body() = loop (i + 1I32, acc + i);
                con exit() = return acc;
            end;
    end;
`,
    'dep.mim': `plugin refly;

// \`Vec\` is an ordinary \`lam\` - but it returns a *type*: it is a function \`Nat → *\`.
extern lam Vec (n: Nat): * = «n; Nat»;

// \`zeros n\` builds a length-\`n\` array of zeros.
// Its return *type* \`Vec n\` mentions the *value* \`n\`: a dependent function type.
extern lam zeros (n: Nat): Vec n = ‹n; 0›;

// \`zeros 3\` is partial-evaluated to \`‹3; 0›\` during graph construction; assert it statically.
let _ = refly.equiv.struc_eq (zeros 3, ‹3; 0›);
`,
    'iter.mim': `plugin core;
plugin refly;

use core.ops.n;

extern lam iter {T: *} (f: T → T) (n: Nat, x: T)@(core.pe.is_closed n): T =
    (core.select ((n <= 0), cons, alt)) () where
        lam cons(): T =
            x;
        lam alt(): T =
            let m = n - 1;
            let y = f x;
            iter @T f (m, y);
    end;

lam succ (x: Nat): Nat = x + 1;
lam add  (x: Nat) (y: Nat): Nat = iter succ (x, y);
lam mul  (x: Nat) (y: Nat): Nat = iter (add x) (y, 0);
lam pow  (x: Nat) (y: Nat): Nat = iter (mul x) (y, 1);
let _ = refly.equiv.struc_eq (pow 3 5, 243);
`,
};

const $ = id => document.getElementById(id);
const status = $('status');

// Instantiating the module per run keeps each compile in a pristine World; compiling the wasm
// itself only once makes that cost a few milliseconds instead of a fresh parse of 2.4MB.
let compiled = null;
async function freshMim(onErr) {
    const opts = { print: onErr, printErr: onErr, noExitRuntime: true };
    if (compiled) {
        opts.instantiateWasm = (imports, receive) =>
            WebAssembly.instantiate(compiled, imports).then(i => receive(i, compiled));
    } else {
        opts.instantiateWasm = (imports, receive) =>
            WebAssembly.instantiateStreaming(fetch('mim.wasm'), imports).then(({ module, instance }) => {
                compiled = module;
                receive(instance, module);
            });
    }
    return createMim(opts);
}

let graphviz = null;
let running = false;
let queued = false;

async function run() {
    if (running) { queued = true; return; }
    running = true;
    status.className = '';
    status.textContent = 'running…';

    const src = getSource();
    const log = [];
    const t0 = performance.now();
    let code = -1;

    try {
        const M = await freshMim(s => log.push(s));
        M.FS.writeFile('/in.mim', src);
        const args = ['/in.mim', '-P', '/mim', '--output-dot', '/out.dot', '-o', '/out.mim'];
        if ($('optimize').checked) args.push('-p', 'opt', '-p', 'll', '-X', 'll:o=/out.ll');
        else args.push('--no-opt');

        code = M.callMain(args);
        show('mim', read(M, '/out.mim'));
        show('ll', read(M, '/out.ll') ?? '(enable "optimize" to run the ll backend)');
        await showGraph(read(M, '/out.dot'));
    } catch (e) {
        log.push(/call stack/.test(e?.message ?? '')
            ? 'wasm stack exhausted - this program recurses deeper than the browser stack allows'
            : String(e?.message ?? e));
    }

    show('log', log.join('\n') || '(no diagnostics)');
    const ms = Math.round(performance.now() - t0);
    const failed = code !== 0;
    status.className = failed ? 'error' : '';
    status.textContent = failed ? `failed - see Log (${ms} ms)` : `ok (${ms} ms)`;
    if (failed) select('log');

    running = false;
    if (queued) { queued = false; run(); }
}

function read(M, path) {
    try { return M.FS.readFile(path, { encoding: 'utf8' }); } catch { return null; }
}

function show(pane, text) {
    $(`pane-${pane}`).querySelector('pre').textContent = text ?? '';
}

async function showGraph(dot) {
    const pane = $('pane-graph');
    if (!dot) { pane.textContent = '(no graph)'; return; }
    graphviz ??= await Graphviz.load();
    pane.innerHTML = graphviz.layout(dot, 'svg', 'dot');

    // Graphviz sizes the SVG in points; drop that so the viewBox scales it to whatever the pane is.
    const svg = pane.querySelector('svg');
    svg?.removeAttribute('width');
    svg?.removeAttribute('height');
}

function select(pane) {
    for (const tab of document.querySelectorAll('#tabs button')) {
        const on = tab.dataset.pane === pane;
        tab.setAttribute('aria-selected', String(on));
        $(`pane-${tab.dataset.pane}`).hidden = !on;
    }
}

/*
 * editor
 */

let editor = null; // CodeMirror, if it loads; the <textarea> is the fallback
const getSource = () => editor ? editor.state.doc.toString() : $('source').value;

function setSource(text) {
    if (editor) editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: text } });
    else $('source').value = text;
}

// One copy of @codemirror/state must back every extension, so the packages are imported
// individually from a CDN that shares their dependencies rather than as pre-bundled `+esm` blobs,
// which ship a private copy each and fail CodeMirror's instanceof checks.
async function setupEditor(initial) {
    $('source').value = initial;
    try {
        const cdn = 'https://esm.sh/@codemirror/';
        const [view, language, commands] = await Promise.all(
            ['view@6', 'language@6', 'commands@6'].map(pkg => import(cdn + pkg)));

        editor = new view.EditorView({
            doc: initial,
            extensions: [
                view.lineNumbers(),
                view.highlightActiveLine(),
                view.drawSelection(),
                commands.history(),
                view.keymap.of([...commands.defaultKeymap, ...commands.historyKeymap]),
                language.syntaxHighlighting(language.defaultHighlightStyle, { fallback: true }),
                language.StreamLanguage.define(mimMode()),
                view.EditorView.updateListener.of(u => u.docChanged && schedule()),
            ],
            parent: $('editor'),
        });
        $('source').remove();
    } catch (e) {
        console.warn('CodeMirror unavailable, falling back to a plain textarea:', e);
        $('source').addEventListener('input', schedule);
    }
}

// Enough of Mim to colour a snippet; the real grammar lives in the compiler.
function mimMode() {
    const keywords = new Set(['plugin', 'import', 'use', 'let', 'lam', 'con', 'fun', 'cn', 'fn', 'axm',
                              'extern', 'where', 'end', 'return', 'rec', 'ret', 'as', 'Nat', 'Idx', 'Bool']);
    return {
        token(stream) {
            if (stream.match(/^\/\/.*/)) return 'comment';
            if (stream.match(/^\/\*/)) { stream.skipTo('*/') && stream.match(/^\*\//); return 'comment'; }
            if (stream.match(/^"(?:[^"\\]|\\.)*"?/)) return 'string';
            if (stream.match(/^[0-9][0-9_]*(?:\.[0-9]*)?(?:[IiUuFf][0-9]+)?/)) return 'number';
            if (stream.match(/^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*/)) {
                const w = stream.current().split('.')[0];
                return keywords.has(w) ? 'keyword' : /^[A-Z]/.test(w) ? 'typeName' : 'variableName';
            }
            if (stream.match(/^[⊤⊥«»‹›→←λΠ∀]/)) return 'operator';
            stream.next();
            return null;
        },
        languageData: { commentTokens: { line: '//', block: { open: '/*', close: '*/' } } },
    };
}

let timer = 0;
function schedule() {
    clearTimeout(timer);
    timer = setTimeout(run, 500);
}

/*
 * boot
 */

const picker = $('examples');
for (const name of Object.keys(EXAMPLES)) picker.add(new Option(name, name));
picker.onchange = () => { setSource(EXAMPLES[picker.value]); run(); };
$('run').onclick = run;
$('optimize').onchange = run;
for (const tab of document.querySelectorAll('#tabs button')) tab.onclick = () => select(tab.dataset.pane);

await setupEditor(EXAMPLES['sq.mim']);
await run();
