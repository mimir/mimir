import { Graphviz } from 'https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1/dist/index.js';

// Staged from `lit/docs` by web/CMakeLists.txt, so the examples are the ones the lit suite covers.
const EXAMPLES = ['sq.mim', 'count.mim', 'dep.mim', 'iter.mim'];

const $ = id => document.getElementById(id);
const status = $('status');

const sources = new Map();

async function example(name) {
    if (!sources.has(name)) {
        const text = await fetch(`examples/${name}`).then(r => r.text());
        sources.set(name, text.replace(/^\/\/ RUN:.*\n/gm, ''));
    }
    return sources.get(name);
}

/*
 * compiler
 */

// Only terminate() stops a wasm loop that never returns, and only a Worker can be terminated.
const LOAD_TIMEOUT = 60_000; // the first run also pays for the 2.4MB download
const RUN_TIMEOUT  = 10_000;

let worker = null;
let inflight = null;

function compile(src, args) {
    return new Promise((resolve, reject) => {
        worker ??= spawn();
        inflight = { resolve, reject, timer: 0 };
        wait(LOAD_TIMEOUT, 'the compiler did not load');
        worker.postMessage({ src, args });
    });
}

function spawn() {
    const w = new Worker('mim-worker.js');
    w.onmessage = ({ data }) => data.ready
        ? wait(RUN_TIMEOUT, `gave up after ${RUN_TIMEOUT / 1000} s - this program may not terminate`)
        : settle(f => f.resolve(data));
    w.onerror = e => abort(e.message || 'the compiler worker died');
    return w;
}

function wait(ms, why) {
    if (!inflight) return;
    clearTimeout(inflight.timer);
    inflight.timer = setTimeout(() => abort(why), ms);
}

// A hung worker stays hung, so drop it; the next run spawns a fresh one.
function abort(why) {
    worker?.terminate();
    worker = null;
    settle(f => f.reject(new Error(why)));
}

function settle(f) {
    const cur = inflight;
    if (!cur) return;
    inflight = null;
    clearTimeout(cur.timer);
    f(cur);
}

/*
 * run
 */

// Downloading Graphviz overlaps the first compile instead of following it.
const graphvizReady = Graphviz.load();
let running = false;
let queued = false;

async function run() {
    if (running) { queued = true; return; }
    running = true;
    $('run').textContent = 'Stop';
    status.className = '';
    status.textContent = 'running…';

    const args = ['/in.mim', '-P', '/mim', '--output-dot', '/out.dot', '-o', '/out.mim'];
    for (const box of document.querySelectorAll('#dot-opts input:checked')) args.push(`--dot-${box.dataset.dot}`);
    if ($('optimize').checked) args.push('-p', 'opt', '-p', 'll', '-X', 'll:o=/out.ll');
    else args.push('--no-opt');

    const t0 = performance.now();
    let log = [];
    let code = -1;
    let why = 'failed - see Log';

    try {
        const res = await compile(getSource(), args);
        ({ code, log } = res);
        if (res.error) log.push((why = res.error));
        show('mim', res.out?.mim);
        show('ll', res.out?.ll ?? '(enable "optimize" to run the ll backend)');
        await showGraph(res.out?.dot);
    } catch (e) {
        log.push((why = String(e?.message ?? e)));
    }

    showLog(log.join('\n') || '(no diagnostics)');
    const ms = Math.round(performance.now() - t0);
    const failed = code !== 0;
    status.className = failed ? 'error' : '';
    status.textContent = failed ? `${why} (${ms} ms)` : `ok (${ms} ms)`;
    if (failed) select('log');

    running = false;
    $('run').textContent = 'Run';
    if (queued) { queued = false; run(); }
}

function show(pane, text) {
    $(`pane-${pane}`).querySelector('pre').textContent = text ?? '';
}

// mim colours its diagnostics with SGR escapes; a foreground colour replaces the previous one
// rather than nesting, so every escape closes the open span before opening the next.
function showLog(text) {
    let html = '';
    let open = false;
    let last = 0;

    for (const m of text.matchAll(/\x1b\[([0-9;]*)m/g)) {
        html += MimCode.escape(text.slice(last, m.index));
        last  = m.index + m[0].length;
        if (open) html += '</span>';
        const code = Number(m[1].split(';').pop() || 0);
        open = (code >= 30 && code <= 36) || code === 90;
        if (open) html += `<span class="fg${code}">`;
    }

    $('pane-log').querySelector('pre').innerHTML
        = html + MimCode.escape(text.slice(last)) + (open ? '</span>' : '');
}

// Layout runs on the page, so a graph big enough to freeze it is refused rather than attempted.
const MAX_DOT = 512 * 1024;

let dot = null; // laid out only while the Graph tab is up, since layout blocks the editor

async function showGraph(latest) {
    dot = latest;
    if (!$('pane-graph').hidden) await layoutGraph();
}

async function layoutGraph() {
    const pane = $('graph');
    if (!dot) { pane.textContent = '(no graph)'; return; }
    if (dot.length > MAX_DOT) {
        pane.textContent = `(${Math.round(dot.length / 1024)} KB of DOT - too large to lay out here)`;
        return;
    }
    const graphviz = await graphvizReady;
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
    if (pane === 'graph') layoutGraph();
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

// Classifies against the docs' word lists (mim-code.js), so both stay in step with `ast/family.h`.
function mimMode() {
    return {
        token(stream) {
            if (stream.match(/^\/\/.*/)) return 'comment';
            if (stream.match(/^\/\*/)) { stream.skipTo('*/') && stream.match(/^\*\//); return 'comment'; }
            if (stream.match(/^"(?:[^"\\]|\\.)*"?/)) return 'string';
            if (stream.match(/^[0-9][0-9_]*(?:\.[0-9]*)?(?:[IiUuFf][0-9]+)?/)) return 'number';
            if (stream.match(/^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*/)) {
                const w = stream.current().split('.')[0];
                if (MimCode.KEYWORD.has(w) || MimCode.DECL.has(w) || MimCode.SPECIAL.has(w)) return 'keyword';
                if (MimCode.TYPE.has(w)) return 'typeName';
                if (MimCode.LITERAL.has(w)) return 'atom';
                return /^[A-Z]/.test(w) ? 'typeName' : 'variableName';
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
for (const name of EXAMPLES) picker.add(new Option(name, name));
picker.onchange = async () => { setSource(await example(picker.value)); run(); };
$('run').onclick = () => { if (running) { queued = false; abort('stopped'); } else run(); };
$('optimize').onchange = run;
$('dot-opts').onchange = run;
for (const tab of document.querySelectorAll('#tabs button')) tab.onclick = () => select(tab.dataset.pane);

// setupEditor fills the textarea before it awaits, so the first compile can start alongside it.
const initial = await example(EXAMPLES[0]);
await Promise.all([setupEditor(initial), run()]);
