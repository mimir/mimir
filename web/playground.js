import { Graphviz } from 'https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1/dist/index.js';
import { GraphView } from './graphview.js';

const $ = id => document.getElementById(id);
const status = $('status');

// Written by web/CMakeLists.txt from the list that stages the examples.
const EXAMPLES = await fetch('examples/index.json').then(r => r.json());

// The `RUN:`/`CHECK:` lines make these lit tests; they are noise in the editor.
const example = name => fetch(`examples/${name}.mim`)
    .then(r => r.text())
    .then(text => text.replace(/^\/\/ (RUN|CHECK[\w-]*):.*\n/gm, '').trim() + '\n');

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

    const args = ['/in.mim', '-P', '/mim', '--output-dot', '/out.dot', '--output-ast', '/out.ast', '-o', '/out.mim'];
    for (const box of document.querySelectorAll('#dot-opts input:checked')) args.push(`--dot-${box.dataset.dot}`);
    if (dark) args.push('--dot-dark');
    for (const box of document.querySelectorAll('#mim-opts input[data-mim]:checked')) args.push(`--mim-${box.dataset.mim}`);
    if ($('ascii').checked) args.push('-a');
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
        showCode('mim', MimCode, res.out?.mim);
        showCode('ast', MimCode, res.out?.ast);
        if (res.out?.ll) showCode('ll', LlvmCode, res.out.ll);
        else showCode('ll', null, '(enable "optimize" to run the ll backend)');
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

// The docs' lexers colour these panes, too - but onto the palette of this page.
const CLASS = { comment: 'tok-comment', meta: 'tok-comment', string: 'tok-string', number: 'tok-number',
                keyword: 'tok-keyword', decl: 'tok-keyword', type: 'tok-type', literal: 'tok-literal',
                special: 'tok-special', global: 'tok-special', label: 'tok-special' };
MimCode.CLASS = LlvmCode.CLASS = CLASS;

// Above this the lexer costs more than the colours are worth.
const MAX_CODE = 256 * 1024;

const pending = {}; // pane -> the {lexer, text} it should show; lexed only while up: a big dump blocks the editor
const shown = {};   // pane -> the `text` the pane already shows

function showCode(pane, lexer, text) {
    pending[pane] = { lexer, text: text ?? '' };
    if (!$(`pane-${pane}`).hidden) renderCode(pane);
}

function renderCode(pane) {
    const { lexer, text } = pending[pane];
    if (shown[pane] === text) return;
    shown[pane] = text;
    const pre = $(`pane-${pane}`).querySelector('pre');
    if (lexer && text && text.length <= MAX_CODE) pre.innerHTML = lexer.code(text);
    else pre.textContent = text;
}

// An SGR colour replaces the previous one rather than nesting, so every escape closes the open span.
function showLog(text) {
    let html = '';
    let open = false;
    let last = 0;

    for (const m of text.matchAll(/\x1b\[([0-9;]*)m/g)) {
        html += Code.escape(text.slice(last, m.index));
        last  = m.index + m[0].length;
        if (open) html += '</span>';
        const code = Number(m[1].split(';').pop() || 0);
        open = (code >= 30 && code <= 36) || code === 90;
        if (open) html += `<span class="fg${code}">`;
    }

    $('pane-log').querySelector('pre').innerHTML
        = html + Code.escape(text.slice(last)) + (open ? '</span>' : '');
}

// Layout runs on the page, so a graph big enough to freeze it is refused.
const MAX_DOT = 512 * 1024;

const graph = new GraphView($('graph'), $('graph-nav'));

let dot = null;  // laid out only while the Graph tab is up or a pop-out is watching: layout blocks the editor
let laidOut;     // the `dot` the pane already shows
let svg = null;  // what came out of the layout, or null with `msg` saying why there is none
let msg = '';

async function showGraph(latest) {
    dot = latest;
    if (!$('pane-graph').hidden || poppedOut()) await layoutGraph();
}

async function layoutGraph() {
    if (dot === laidOut) return post();
    laidOut = dot;
    if (!dot) return render(null, '(no graph)');
    if (dot.length > MAX_DOT) return render(null, `(${Math.round(dot.length / 1024)} KB of DOT - too large to lay out here)`);
    const graphviz = await graphvizReady;
    if (dot !== laidOut) return; // superseded while Graphviz was still loading
    render(graphviz.layout(dot, 'svg', 'dot'));
}

function render(latest, why) {
    svg = latest;
    msg = why;
    svg ? graph.render(svg) : graph.message(msg);
    post();
}

// The pop-out shows the very SVG this page laid out, so it needs neither Graphviz nor the compiler.
let popout = null;
const poppedOut = () => popout && !popout.closed;

function post() {
    if (poppedOut()) popout.postMessage({ svg, msg, dark }, '*');
}

function popOut() {
    if (poppedOut()) return popout.focus();
    popout = window.open('graph.html', 'mim-graph', 'popup,width=1000,height=800');
}

window.addEventListener('message', e => { if (e.source === popout && e.data?.ready) layoutGraph(); });

function select(pane) {
    for (const tab of document.querySelectorAll('#tabs button')) {
        const on = tab.dataset.pane === pane;
        tab.setAttribute('aria-selected', String(on));
        $(`pane-${tab.dataset.pane}`).hidden = !on;
    }
    if (pane === 'graph') layoutGraph();
    else if (pending[pane]) renderCode(pane);
}

let editor = null; // CodeMirror, if it loads; the <textarea> is the fallback
let viSlot = null; // the compartment the vi keymap is swapped in and out of
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
        const [state, view, language, commands, { Tag }] = await Promise.all(
            ['state@6', 'view@6', 'language@6', 'commands@6'].map(pkg => import(cdn + pkg))
                .concat(import('https://esm.sh/@lezer/highlight@1')));
        const tokens = Object.fromEntries(Object.keys(CLASS).map(kind => [kind, Tag.define()]));

        viSlot = new state.Compartment();
        editor = new view.EditorView({
            doc: initial,
            extensions: [
                viSlot.of([]), // vi rebinds Esc and the printable keys, so it has to outrank the keymaps below
                view.lineNumbers(),
                view.highlightActiveLine(),
                view.drawSelection(),
                commands.history(),
                view.keymap.of([...commands.defaultKeymap, ...commands.historyKeymap]),
                language.syntaxHighlighting(language.HighlightStyle.define(
                    Object.entries(CLASS).map(([kind, cls]) => ({ tag: tokens[kind], class: cls })))),
                language.StreamLanguage.define(mimMode(tokens)),
                view.EditorView.updateListener.of(u => u.docChanged && schedule()),
            ],
            parent: $('editor'),
        });
        $('source').remove();
    } catch (e) {
        console.warn('CodeMirror unavailable, falling back to a plain textarea:', e);
        $('source').addEventListener('input', schedule);
        $('vi').closest('label').remove();
    }
}

const VI_KEY = 'mim-playground-vi';
let vi = null; // @replit/codemirror-vim, fetched when the box is first ticked

async function toggleVi() {
    const on = $('vi').checked;
    localStorage.setItem(VI_KEY, on ? '1' : '');
    if (!viSlot) return;
    try {
        if (on && !vi) {
            vi = await import('https://esm.sh/@replit/codemirror-vim@6');
            vi.Vim.defineEx('write', 'w', run);
        }
        editor.dispatch({ effects: viSlot.reconfigure(on ? vi.vim({ status: true }) : []) });
        editor.focus();
    } catch (e) {
        console.warn('vi mode unavailable:', e);
        $('vi').checked = false;
    }
}

// mim-code.js does the classifying and CLASS the colouring, so the editor matches the code panes.
function mimMode(tokens) {
    return {
        startState: () => ({ comment: false }),
        token(stream, state) {
            const { end, kind } = MimCode.next(stream.string, stream.pos, state);
            stream.pos = end;
            return kind in tokens ? kind : null;
        },
        tokenTable: tokens,
        languageData: { commentTokens: { line: '//', block: { open: '/*', close: '*/' } } },
    };
}

let timer = 0;
function schedule() {
    clearTimeout(timer);
    timer = setTimeout(run, 500);
}

// Read raw: URLSearchParams would turn a `+` operator into a space.
function urlSource() {
    const m = /[?&]src=([^&]*)/.exec(location.search);
    if (!m) return null;
    try { return decodeURIComponent(m[1]); } catch { return m[1]; }
}

function shareLink() {
    const url = new URL(location.href);
    url.search = 'src=' + encodeURIComponent(getSource());
    return url.href;
}

// navigator.clipboard needs a secure context, which a plain http:// page is not.
async function copy(text) {
    try {
        return await navigator.clipboard.writeText(text);
    } catch {}
    const ta = document.createElement('textarea');
    ta.value = text;
    document.body.append(ta);
    ta.select();
    const ok = document.execCommand('copy');
    ta.remove();
    if (!ok) throw new Error('could not copy');
}

async function share() {
    const btn = $('share');
    try {
        await copy(shareLink());
        btn.textContent = 'Link copied!';
    } catch {
        btn.textContent = 'Copy failed';
    }
    setTimeout(() => (btn.textContent = 'Share'), 1500);
}

const custom = urlSource();
const picker = $('examples');
if (custom !== null) picker.add(new Option('(custom)', ''));
for (const name of EXAMPLES) picker.add(new Option(`${name}.mim`, name));
picker.onchange = async () => { setSource(picker.value ? await example(picker.value) : custom); run(); };
$('run').onclick = () => { if (running) { queued = false; abort('stopped'); } else run(); };
$('share').onclick = share;
$('vi').checked = localStorage.getItem(VI_KEY) === '1';
$('vi').onchange = toggleVi;
$('optimize').onchange = run;
$('dot-opts').onchange = run;
$('mim-opts').onchange = run;
$('popout').onclick = popOut;
for (const tab of document.querySelectorAll('#tabs button')) tab.onclick = () => select(tab.dataset.pane);

// darkmode-toggle.js owns the preference and the <html> class; the graph is baked by the compiler, so a
// theme change - by click or by the system flipping underneath us - has to recompile.
let dark = document.documentElement.classList.contains('dark-mode');
$('theme').updateIcon();

new MutationObserver(() => {
    $('theme').updateIcon();
    const now = document.documentElement.classList.contains('dark-mode');
    if (now !== dark) { dark = now; run(); }
}).observe(document.documentElement, { attributeFilter: ['class'] });

// setupEditor fills the textarea before it awaits, so the first compile starts alongside.
const initial = custom ?? await example(EXAMPLES[0]);
await Promise.all([setupEditor(initial), run()]);
if (editor && $('vi').checked) await toggleVi();
