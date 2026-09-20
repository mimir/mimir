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
    if (dark) args.push('--dot-dark');
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
    if (!dot) { pane.textContent = '(no graph)'; return attach(null); }
    if (dot.length > MAX_DOT) {
        pane.textContent = `(${Math.round(dot.length / 1024)} KB of DOT - too large to lay out here)`;
        return attach(null);
    }
    const graphviz = await graphvizReady;
    if (dot !== laidOut) return; // superseded while Graphviz was still loading
    pane.innerHTML = graphviz.layout(dot, 'svg', 'dot');

    // Graphviz sizes the SVG in points; drop that so the viewBox scales it to the pane.
    const svg = pane.querySelector('svg');
    svg?.removeAttribute('width');
    svg?.removeAttribute('height');
    attach(svg);
}

// The viewBox already fits the graph to the pane, so identity is "fit" and pan/zoom rides on top of it.
const ZOOM_MIN = 0.2, ZOOM_MAX = 50, ZOOM_STEP = 1.25;

let view = null; // the <g> carrying the pan/zoom transform, or null while no graph is up
let tx, ty, k;

// A re-run keeps the current view: the editor recompiles on every pause in typing, and snapping back
// to fit each time would make zooming pointless.
function attach(svg) {
    const fresh = !view;
    view = null;
    if (!svg) return;
    view = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    view.append(...svg.childNodes);
    svg.append(view);
    fresh ? fit() : apply();
    index(svg);
}

function fit() {
    tx = ty = 0;
    k = 1;
    apply();
}

function apply() {
    view.setAttribute('transform', `translate(${tx} ${ty}) scale(${k})`);
}

// getScreenCTM maps the SVG's own coordinates - the ones tx/ty/k live in - onto the screen.
const toGraph = (x, y) => new DOMPoint(x, y).matrixTransform(view.ownerSVGElement.getScreenCTM().inverse());

function zoom(factor, x, y) {
    const next = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, k * factor));
    const p = toGraph(x, y);
    factor = next / k;
    tx = p.x - (p.x - tx) * factor;
    ty = p.y - (p.y - ty) * factor;
    k = next;
    apply();
}

// Keeps the point under the pointer put; the buttons and keys zoom around the pane's centre instead.
function zoomCenter(factor) {
    const r = $('graph').getBoundingClientRect();
    zoom(factor, r.left + r.width / 2, r.top + r.height / 2);
}

function pan(e) {
    if (!view || e.button !== 0) return;
    const graph = $('graph');
    const scale = view.ownerSVGElement.getScreenCTM().a;
    let { clientX: x, clientY: y } = e;
    dragging = true;
    dragged = false;

    const move = e => {
        dragged ||= Math.abs(e.clientX - x) + Math.abs(e.clientY - y) > 2;
        tx += (e.clientX - x) / scale;
        ty += (e.clientY - y) / scale;
        [x, y] = [e.clientX, e.clientY];
        apply();
    };
    const drop = () => {
        graph.removeEventListener('pointermove', move);
        graph.classList.remove('grabbing');
        dragging = false;
    };

    graph.classList.add('grabbing');
    graph.addEventListener('pointermove', move);
    graph.addEventListener('pointerup', drop, { once: true });
    graph.addEventListener('pointercancel', drop, { once: true });
    graph.setPointerCapture(e.pointerId); // last: it only extends the drag past the pane's edge
}

// A notch is 120 px; Firefox reports lines instead, where 3 lines are that same notch.
function wheel(e) {
    if (!view) return;
    e.preventDefault();
    zoom(ZOOM_STEP ** (-(e.deltaMode ? e.deltaY * 40 : e.deltaY) / 120), e.clientX, e.clientY);
}

function key(e) {
    if (!view) return;
    if (e.key === '+' || e.key === '=') zoomCenter(ZOOM_STEP);
    else if (e.key === '-') zoomCenter(1 / ZOOM_STEP);
    else if (e.key === '0') fit();
    else if (e.key === 'Escape') pin(null);
    else return;
    e.preventDefault();
}

// A node's <title> is its dot id; an edge's is `<tail>:<port>-><head>`.
const nid = g => g.querySelector('title')?.textContent ?? '';

let adj = new Map();  // id -> {g, edges: [{g, other}]}
let focused = null;   // the id the highlight is on, pinned or merely hovered
let pinned = null;
let dragging = false;
let dragged = false;

function index(svg) {
    adj = new Map();
    const node = id => adj.get(id) ?? adj.set(id, { g: null, edges: [] }).get(id);

    for (const g of svg.querySelectorAll('g.node')) node(nid(g)).g = g;
    for (const g of svg.querySelectorAll('g.edge')) {
        const [from, to] = nid(g).split('->').map(s => s.replace(/:.*$/, ''));
        node(from).edges.push({ g, other: to });
        node(to).edges.push({ g, other: from });
    }

    focused = null;
    if (pinned && !adj.has(pinned)) pinned = null; // a recompile may have renumbered it away
    show(pinned);
}

// Dims all but a node, its direct neighbours, and the edges between. The highlight outranks the
// `stroke="none"` of a detached edge, so hovering reveals what `hidden edges` would have to recompile for.
function show(id) {
    const svg = view?.ownerSVGElement;
    if (!svg || focused === id) return;
    focused = id;

    for (const g of svg.querySelectorAll('.on, .hub')) g.classList.remove('on', 'hub');
    svg.classList.toggle('focus', id !== null);
    if (id === null) return;

    const hub = adj.get(id);
    hub?.g?.classList.add('on', 'hub');
    for (const { g, other } of hub?.edges ?? []) {
        g.classList.add('on');
        adj.get(other)?.g?.classList.add('on');
    }
}

function pin(id) {
    pinned = id;
    show(id);
}

const nodeAt = e => {
    const g = e.target.closest?.('g.node');
    return g ? nid(g) : null;
};

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
$('optimize').onchange = run;
$('dot-opts').onchange = run;
$('mim-opts').onchange = run;
$('graph-nav').onclick = e => {
    const how = e.target.dataset.zoom;
    if (!view || !how) return;
    if (how === 'fit') fit();
    else zoomCenter(how === 'in' ? ZOOM_STEP : 1 / ZOOM_STEP);
};
$('graph').addEventListener('pointerdown', pan);
$('graph').addEventListener('mousemove', e => { if (!pinned && !dragging) show(nodeAt(e)); });
$('graph').addEventListener('mouseleave', () => { if (!pinned) show(null); });
$('graph').addEventListener('click', e => {
    if (dragged) return;
    const id = nodeAt(e);
    pin(id && id !== pinned ? id : null);
});
$('graph').addEventListener('wheel', wheel, { passive: false });
$('graph').addEventListener('dblclick', () => view && fit());
$('graph').addEventListener('keydown', key);
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
