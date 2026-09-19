import { Graphviz } from 'https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1/dist/index.js';

const $ = id => document.getElementById(id);
const status = $('status');

// Written by web/CMakeLists.txt from the list that stages the examples.
const EXAMPLES = await fetch('examples/index.json').then(r => r.json());

// The `RUN:` line makes these lit tests; it is noise in the editor.
const example = name => fetch(`examples/${name}.mim`)
    .then(r => r.text())
    .then(text => text.replace(/^\/\/ RUN:.*\n/gm, ''));

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

// A hung worker stays hung; the next run spawns a fresh one.
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

// Overlaps the Graphviz download with the first compile.
const graphvizReady = Graphviz.load();
let running = false;
let queued = false;

async function run() {
    clearTimeout(timer);
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

// An SGR colour replaces the previous one rather than nesting, so every escape closes the open span.
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

// Layout runs on the page, so a graph big enough to freeze it is refused.
const MAX_DOT = 512 * 1024;

let dot = null;  // laid out only while the Graph tab is up: layout blocks the editor
let laidOut;     // the `dot` the pane already shows

async function showGraph(latest) {
    dot = latest;
    if (!$('pane-graph').hidden) await layoutGraph();
}

async function layoutGraph() {
    const pane = $('graph');
    if (dot === laidOut) return;
    laidOut = dot;
    if (!dot) { pane.textContent = '(no graph)'; return; }
    if (dot.length > MAX_DOT) {
        pane.textContent = `(${Math.round(dot.length / 1024)} KB of DOT - too large to lay out here)`;
        return;
    }
    const graphviz = await graphvizReady;
    if (dot !== laidOut) return; // superseded while Graphviz was still loading
    pane.innerHTML = graphviz.layout(dot, 'svg', 'dot');

    // Graphviz sizes the SVG in points; drop that so the viewBox scales it to the pane.
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

let editor = null; // CodeMirror, if it loads; the <textarea> is the fallback
const getSource = () => editor ? editor.state.doc.toString() : $('source').value;

function setSource(text) {
    if (editor) editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: text } });
    else $('source').value = text;
}

// One copy of @codemirror/state must back every extension, so import the packages individually from
// a CDN that shares dependencies; `+esm` bundles ship a private copy each and break instanceof.
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

// mim-code.js does the classifying, so both stay in step with `ast/family.h`.
const TAG = { comment: 'comment', string: 'string', number: 'number', keyword: 'keyword', decl: 'keyword',
              type: 'typeName', literal: 'atom', special: 'keyword', operator: 'operator' };

function mimMode() {
    return {
        startState: () => ({ comment: false }),
        token(stream, state) {
            const first = stream.string[stream.pos];
            const { end, kind } = MimCode.next(stream.string, stream.pos, state);
            stream.pos = end;
            if (kind === 'name') return /[A-Z]/.test(first) ? 'typeName' : 'variableName';
            return TAG[kind] ?? null;
        },
        languageData: { commentTokens: { line: '//', block: { open: '/*', close: '*/' } } },
    };
}

let timer = 0;
function schedule() {
    clearTimeout(timer);
    timer = setTimeout(run, 500);
}

const picker = $('examples');
for (const name of EXAMPLES) picker.add(new Option(`${name}.mim`, name));
picker.onchange = async () => { setSource(await example(picker.value)); run(); };
$('run').onclick = () => { if (running) { queued = false; abort('stopped'); } else run(); };
$('optimize').onchange = run;
$('dot-opts').onchange = run;
for (const tab of document.querySelectorAll('#tabs button')) tab.onclick = () => select(tab.dataset.pane);

// setupEditor fills the textarea before it awaits, so the first compile starts alongside.
const initial = await example(EXAMPLES[0]);
await Promise.all([setupEditor(initial), run()]);
