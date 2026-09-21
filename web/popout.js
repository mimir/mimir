import { GraphView } from './graphview.js';

// The playground lays the DOT out once and ships the SVG here, so this window needs no Graphviz of its own.
const graph = new GraphView(document.getElementById('graph'), document.getElementById('graph-nav'));

window.addEventListener('message', ({ source, data }) => {
    if (source !== window.opener) return;
    // The theme is baked into the SVG by the compiler, so follow the page that baked it.
    document.documentElement.classList.toggle('dark-mode', !!data.dark);
    document.documentElement.classList.toggle('light-mode', !data.dark);
    data.svg ? graph.render(data.svg) : graph.message(data.msg ?? '(no graph)');
});

// A reload keeps the opener, so asking again is all it takes to get the current graph back.
window.opener?.postMessage({ ready: true }, '*');
document.getElementById('graph').focus();
