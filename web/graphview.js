// Pan, zoom and highlight over a laid-out Graphviz SVG; the Graph tab and the pop-out window share one.

// The viewBox already fits the graph to the pane, so identity is "fit" and pan/zoom rides on top of it.
const ZOOM_MIN = 0.2, ZOOM_MAX = 50, ZOOM_STEP = 1.25;

// A node's <title> is its dot id; an edge's is `<tail>:<port>-><head>`.
const nid = g => g.querySelector('title')?.textContent ?? '';

export class GraphView {
    #host;
    #view = null;      // the <g> carrying the pan/zoom transform, or null while no graph is up
    #tx; #ty; #k;
    #adj = new Map();  // id -> {g, edges: [{g, other}]}
    #focused = null;   // the id the highlight is on, pinned or merely hovered
    #pinned = null;
    #dragging = false;
    #dragged = false;

    constructor(host, nav) {
        this.#host = host;
        host.addEventListener('pointerdown', e => this.#pan(e));
        host.addEventListener('mousemove', e => { if (!this.#pinned && !this.#dragging) this.#show(this.#nodeAt(e)); });
        host.addEventListener('mouseleave', () => { if (!this.#pinned) this.#show(null); });
        host.addEventListener('click', e => {
            if (this.#dragged) return;
            const id = this.#nodeAt(e);
            this.#pin(id && id !== this.#pinned ? id : null);
        });
        host.addEventListener('wheel', e => this.#wheel(e), { passive: false });
        host.addEventListener('dblclick', () => this.#view && this.fit());
        host.addEventListener('keydown', e => this.#key(e));
        nav?.addEventListener('click', e => this.#nav(e.target.dataset.zoom));
    }

    #nav(how) {
        if (!this.#view || !how) return;
        if (how === 'fit') this.fit();
        else this.#zoomCenter(how === 'in' ? ZOOM_STEP : 1 / ZOOM_STEP);
    }

    // A re-run keeps the current view: the editor recompiles on every pause in typing, and snapping back
    // to fit each time would make zooming pointless.
    render(svgText) {
        const fresh = !this.#view;
        this.#view = null;
        this.#host.innerHTML = svgText;

        // Graphviz sizes the SVG in points; drop that so the viewBox scales it to the pane.
        const svg = this.#host.querySelector('svg');
        if (!svg) return;
        svg.removeAttribute('width');
        svg.removeAttribute('height');

        const view = document.createElementNS('http://www.w3.org/2000/svg', 'g');
        view.append(...svg.childNodes);
        svg.append(view);
        this.#view = view;
        fresh ? this.fit() : this.#apply();
        this.#index(svg);
    }

    message(text) {
        this.#view = null;
        this.#host.textContent = text;
    }

    fit() {
        this.#tx = this.#ty = 0;
        this.#k = 1;
        this.#apply();
    }

    #apply() {
        this.#view.setAttribute('transform', `translate(${this.#tx} ${this.#ty}) scale(${this.#k})`);
    }

    // getScreenCTM maps the SVG's own coordinates - the ones tx/ty/k live in - onto the screen.
    #toGraph(x, y) {
        return new DOMPoint(x, y).matrixTransform(this.#view.ownerSVGElement.getScreenCTM().inverse());
    }

    #zoom(factor, x, y) {
        const next = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, this.#k * factor));
        const p = this.#toGraph(x, y);
        factor = next / this.#k;
        this.#tx = p.x - (p.x - this.#tx) * factor;
        this.#ty = p.y - (p.y - this.#ty) * factor;
        this.#k = next;
        this.#apply();
    }

    // Keeps the point under the pointer put; the buttons and keys zoom around the pane's centre instead.
    #zoomCenter(factor) {
        const r = this.#host.getBoundingClientRect();
        this.#zoom(factor, r.left + r.width / 2, r.top + r.height / 2);
    }

    #pan(e) {
        if (!this.#view || e.button !== 0) return;
        const host = this.#host;
        const scale = this.#view.ownerSVGElement.getScreenCTM().a;
        let { clientX: x, clientY: y } = e;
        this.#dragging = true;
        this.#dragged = false;

        const move = e => {
            this.#dragged ||= Math.abs(e.clientX - x) + Math.abs(e.clientY - y) > 2;
            this.#tx += (e.clientX - x) / scale;
            this.#ty += (e.clientY - y) / scale;
            [x, y] = [e.clientX, e.clientY];
            this.#apply();
        };
        const drop = () => {
            host.removeEventListener('pointermove', move);
            host.classList.remove('grabbing');
            this.#dragging = false;
        };

        host.classList.add('grabbing');
        host.addEventListener('pointermove', move);
        host.addEventListener('pointerup', drop, { once: true });
        host.addEventListener('pointercancel', drop, { once: true });
        host.setPointerCapture(e.pointerId); // last: it only extends the drag past the pane's edge
    }

    // A notch is 120 px; Firefox reports lines instead, where 3 lines are that same notch.
    #wheel(e) {
        if (!this.#view) return;
        e.preventDefault();
        this.#zoom(ZOOM_STEP ** (-(e.deltaMode ? e.deltaY * 40 : e.deltaY) / 120), e.clientX, e.clientY);
    }

    #key(e) {
        if (!this.#view) return;
        if (e.key === '+' || e.key === '=') this.#zoomCenter(ZOOM_STEP);
        else if (e.key === '-') this.#zoomCenter(1 / ZOOM_STEP);
        else if (e.key === '0') this.fit();
        else if (e.key === 'Escape') this.#pin(null);
        else return;
        e.preventDefault();
    }

    #index(svg) {
        this.#adj = new Map();
        const node = id => this.#adj.get(id) ?? this.#adj.set(id, { g: null, edges: [] }).get(id);

        for (const g of svg.querySelectorAll('g.node')) node(nid(g)).g = g;
        for (const g of svg.querySelectorAll('g.edge')) {
            const [from, to] = nid(g).split('->').map(s => s.replace(/:.*$/, ''));
            node(from).edges.push({ g, other: to });
            node(to).edges.push({ g, other: from });
        }

        this.#focused = null;
        if (this.#pinned && !this.#adj.has(this.#pinned)) this.#pinned = null; // a recompile may have renumbered it away
        this.#show(this.#pinned);
    }

    // Dims all but a node, its direct neighbours, and the edges between. The highlight outranks the
    // `stroke="none"` of a detached edge, so hovering reveals what `hidden edges` would have to recompile for.
    #show(id) {
        const svg = this.#view?.ownerSVGElement;
        if (!svg || this.#focused === id) return;
        this.#focused = id;

        for (const g of svg.querySelectorAll('.on, .hub')) g.classList.remove('on', 'hub');
        svg.classList.toggle('focus', id !== null);
        if (id === null) return;

        const hub = this.#adj.get(id);
        hub?.g?.classList.add('on', 'hub');
        for (const { g, other } of hub?.edges ?? []) {
            g.classList.add('on');
            this.#adj.get(other)?.g?.classList.add('on');
        }
    }

    #pin(id) {
        this.#pinned = id;
        this.#show(id);
    }

    #nodeAt(e) {
        const g = e.target.closest?.('g.node');
        return g ? nid(g) : null;
    }
}
